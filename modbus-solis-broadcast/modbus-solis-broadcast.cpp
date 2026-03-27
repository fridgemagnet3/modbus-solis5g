#include <stdio.h>
#include <fcntl.h>
#include <boost/thread.hpp>
#ifndef WIN32
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <errno.h>
#include <termios.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#define closesocket close
#else
#include <io.h>
#pragma warning(disable : 4996)
typedef int socklen_t;
#endif
#include <errno.h>
#include <modbus/modbus.h>
#include <boost/date_time.hpp>
#include <boost/chrono/chrono.hpp>
#include <iostream>
#include <sstream>
#include <cjson/cJSON.h>
#include <vector>
#include <algorithm>
#include "modbus_tcp_adu.h"

// maxm. no of TCP clients we can handle
#define MAX_MODBUS_TCP_CLIENTS 10

//
// Collect power generation data from Solis inverter via modbus, then
// encapsulate it as a limited JSON data packet using the same convention as
// the Solis cloud API, compress and UDP broadcast it out to interested clients
//
// Also allow for external clients to perform arbitrary read/write register
// requests by acting as a modbus/TCP server
//
// This is designed to operate in tandem with the existing Wifi logger
//

// info we're interested in from the inverter - matches JSON names
// used by the Solis API
typedef struct {
  uint16_t batteryCapacitySoc; // (%)
  double   batteryPower; // (kW)
  double   pac;    // generation (kW)
  double   psum;   // grid in/out (kW)
  double   familyLoadPower; // load (kW)
  double   etoday;  // generation today (kW)
} ModbusSolisRegister_t;

static bool Verbose = false;

// array of connected modbus clients
static SOCKET ModbusTCPClients[MAX_MODBUS_TCP_CLIENTS];
// pending client requests
static std::vector<ModbusTcpAdu*> ModbusClientRequests;
// mutex used to lock access to the ModbusClientRequests vector
static boost::mutex ModbusClientMutex;

static bool ServiceModbusTcpClient(SOCKET Sfd);

// read the required registers from modbus
static bool ModBusReadSolisRegisters(const char *Device, ModbusSolisRegister_t *ModbusSolisRegisters, uint8_t Slave = 1)
{
  using namespace boost::posix_time;
  modbus_t *Ctx = ModbusTcpAdu::CreateModbusRtuSession(Device,Slave);
  uint16_t RegBank[16];
  int Rc = 0 ;
  bool Ret = false;

  if (!Ctx)
    return false;

  if (Verbose)
  {
    ptime RequestTime(second_clock::local_time());

    std::cout << std::endl << "Issuing request at " << to_simple_string(RequestTime) << "..." << std::endl;
  }

  memset(ModbusSolisRegisters,0,sizeof(ModbusSolisRegister_t)) ;

  // most of what we need is in a single grouping
  // see RS485_MODBUS-Hybrid-BACoghlan-201811228-1854.pdf
  // These are read in one transaction:
  // 33135: battery status 0=charge, 1=discharge (battery current direction)
  // 33139: Battery capacity SOC
  // 33147: House load power
  // 33149:33150: Battery power
  Rc = modbus_read_input_registers(Ctx, 33135, sizeof(RegBank) / sizeof(uint16_t), RegBank);
  if (Rc == sizeof(RegBank) / sizeof(uint16_t))
  {
    ModbusSolisRegisters->batteryCapacitySoc = RegBank[4]; // 33139
    ModbusSolisRegisters->batteryPower = (RegBank[14] << 16) + RegBank[15];  // 33149:33150
    // 33135 - battery charge status, 0=charge, 1=discharge
    if (RegBank[0])
      ModbusSolisRegisters->batteryPower *= -1;  // if discharging, flip the power
    ModbusSolisRegisters->batteryPower/=1000.0 ; // convert to kW
    ModbusSolisRegisters->familyLoadPower = (double)RegBank[12] / 1000 ; // 33147

    // three further register reads are required to get the remaining info...
    // 33057:33058: Current Generation
    // 33263:33264: Meter total active power
    // 33035 Current generation today
    Rc = modbus_read_input_registers(Ctx, 33057, sizeof(uint32_t) / sizeof(uint16_t), RegBank);
    if (Rc == sizeof(uint32_t) / sizeof(uint16_t))
    {
      uint32_t Generation = (RegBank[0] << 16) + RegBank[1];   // expressed in watts
      ModbusSolisRegisters->pac = (double)Generation / 1000 ;  // return as kW

      Rc = modbus_read_input_registers(Ctx, 33263, sizeof(int32_t) / sizeof(uint16_t), RegBank);
      if (Rc == sizeof(uint32_t) / sizeof(uint16_t))
      {
        int32_t ActivePower = MODBUS_GET_INT32_FROM_INT16(RegBank, 0);
        ModbusSolisRegisters->psum = (double)ActivePower * 0.001;

        Rc = modbus_read_input_registers(Ctx, 33035, sizeof(uint16_t), RegBank);
        if (Rc == sizeof(uint16_t))
        {
          // expressed in 0.1kWh intervals
          ModbusSolisRegisters->etoday = (float)(RegBank[0])*0.1;
          Ret = true;
        }
        else
          printf("modbus_read_input_registers: %s\n", modbus_strerror(errno));
      }
      else
        printf("modbus_read_input_registers: %s\n", modbus_strerror(errno));
    }
    else
      printf("modbus_read_input_registers: %s\n", modbus_strerror(errno));
  }
  else
    printf("modbus_read_input_registers: %s\n", modbus_strerror(errno));

  modbus_close(Ctx);
  modbus_free(Ctx);
  return Ret;
}

// this doesn't work on Win32 because you can't use 'select' on anything but sockets...
#ifndef WIN32
// Sync with the next transfer performed by the datalogger & wait for it to finish
static bool SyncWithLogger(const char *Device)
{
  using namespace boost::posix_time;
  int Fd;
  fd_set FdSet;
  struct timeval TimeOut;
  bool BusIdle = true;
  int Rc ;
  uint8_t ScratchBuf[256] ;
  ptime SyncStart(second_clock::local_time()) ;
  const uint32_t SyncTimeout = 80u ;
  
#ifdef WIN32
  Fd = _open(Device, O_RDONLY | _O_BINARY);
#else
  Fd = open(Device, O_RDONLY);
#endif
  if (Fd < 0)
  {
    perror("Failed to open input");
    return false;
  }
  usleep(10000);
  tcflush(Fd,TCIOFLUSH);

  FD_ZERO(&FdSet);
  FD_SET(Fd, &FdSet);

  if ( Verbose )
    std::cout << std::endl << "Sync with logger at " << to_simple_string(SyncStart) << "..." << std::endl ;

  while (BusIdle)
  {
    // first, wait for the next burst of traffic from the logger, this normally occurs every minute
    TimeOut.tv_sec = SyncTimeout;
    TimeOut.tv_usec = 0;
    Rc = select(Fd + 1, &FdSet, NULL, NULL, &TimeOut);
    if (Rc == 0)
    {
      if (Verbose)
        printf("Timed out waiting for traffic - going ahead anyway...\n");
      break;
    }
    else if (Rc < 0)
    {
      perror("select");
      close(Fd);
      return false;
    }
    else  // data is on the bus, that's what we're waiting for
    {
      // I've seen the poll exit near enough immediately when it's clearly NOT synced with 
      // the logger, possibly there is stale data sat in the buffers. If so, dump the data and try again.
      // This typically happens at the start but can also occur when the data logger does one of it's odd/out of sync
      // things
      if ( TimeOut.tv_sec > 76 )
      {
        ssize_t Bytes = read(Fd, ScratchBuf, sizeof(ScratchBuf)) ;
        if ( Verbose )
          printf("Got data %zu bytes within %lu seconds, re-syncing just to be sure...\n", Bytes, SyncTimeout-TimeOut.tv_sec);
      }
      else
        BusIdle = false;
    }
  }

  ptime WaitIdle(second_clock::local_time()) ;
  auto Elapsed = WaitIdle - SyncStart;

  if ( Verbose )
  {
    std::cout << "Elapsed: " << Elapsed.total_seconds() << "s" << std::endl;
    std::cout << std::endl << "Wait for idle at " << to_simple_string(WaitIdle) << "..." << std::endl ;
  }

  // sit in loop waiting for the bus to become inactive again
  while (!BusIdle)
  {
    // dump any pending data
    if (read(Fd, ScratchBuf, sizeof(ScratchBuf)) < 0)
    {
      perror("read");
      break;
    }

    // wait for ~12s of inactivity
    TimeOut.tv_sec = 12;
    TimeOut.tv_usec = 500*1000;
    Rc = select(Fd + 1, &FdSet, NULL, NULL, &TimeOut);
    if (Rc == 0)
      BusIdle = true;  // timed out, we should now have a good 50s time on the bus
    else if (Rc < 0)
    {
      perror("select");
    }
  }

  if ( Verbose )
  {
    ptime NowIdle(second_clock::local_time()) ;
    Elapsed = NowIdle - WaitIdle ;

    std::cout << "Elapsed: " << Elapsed.total_seconds() << "s" << std::endl;
  }

  close(Fd);

  return BusIdle;
}
#endif

// generate JSON message aligned to Solis API from the register data
static char *GenerateJson(const ModbusSolisRegister_t *ModbusSolisRegisters)
{
  using namespace boost::chrono;
  cJSON *SolarJson = cJSON_CreateObject();
  cJSON *Node;
  cJSON *SolarData;
  char *Ret;
  system_clock::time_point Now(system_clock::now());
  system_clock::duration UnixTime = Now.time_since_epoch();
  std::stringstream TimeBuf;

  if (!SolarJson)
    return nullptr ;

  // response code
  Node = cJSON_CreateString("0");
  if ( Node )
    cJSON_AddItemToObject(SolarJson, "code", Node);

  // create the 'data' block
  SolarData = cJSON_CreateObject();
  if (SolarData)
  {
    cJSON_AddItemToObject(SolarJson, "data", SolarData);

    // Add in a dummy 'storageBatteryCurrent' entry. This serves two purposes...
    // Firstly, my ipcam_snap script expects this to be in the data (even though it doesn't use it)
    // Secondly, James' JSON parser fails to correctly decode the first entry in the packet so we
    // can't include any data that it needs (& this is one such thing) at the start. 
    Node = cJSON_CreateNumber(1);
    if (Node)
      cJSON_AddItemToObject(SolarData, "storageBatteryCurrent", Node);

    // timestamp
    TimeBuf << duration_cast<milliseconds>(UnixTime).count();
    Node = cJSON_CreateString(TimeBuf.str().c_str());
    if (Node)
      cJSON_AddItemToObject(SolarData, "dataTimestamp", Node);

    // "eToday" = solar energy generated today
    Node = cJSON_CreateNumber(ModbusSolisRegisters->etoday);
    if (Node)
      cJSON_AddItemToObject(SolarData, "eToday", Node);
    Node = cJSON_CreateString("kW");
    if (Node)
      cJSON_AddItemToObject(SolarData, "eTodayStr", Node);

    // generation
    Node = cJSON_CreateNumber(ModbusSolisRegisters->pac);
    if (Node)
      cJSON_AddItemToObject(SolarData, "pac", Node);
    Node = cJSON_CreateString("kW");
    if (Node)
      cJSON_AddItemToObject(SolarData, "pacStr", Node);
    // battery capacity
    Node = cJSON_CreateNumber(ModbusSolisRegisters->batteryCapacitySoc);
    if (Node)
      cJSON_AddItemToObject(SolarData, "batteryCapacitySoc", Node);
    // battery power
    Node = cJSON_CreateNumber(ModbusSolisRegisters->batteryPower);
    if (Node)
      cJSON_AddItemToObject(SolarData, "batteryPower", Node);
    Node = cJSON_CreateString("kW");
    if (Node)
      cJSON_AddItemToObject(SolarData, "batteryPowerStr", Node);

    // grid in/out
    Node = cJSON_CreateNumber(ModbusSolisRegisters->psum);
    if (Node)
      cJSON_AddItemToObject(SolarData, "psum", Node);
    Node = cJSON_CreateString("kW");
    if (Node)
      cJSON_AddItemToObject(SolarData, "psumStr", Node);
    // load
    Node = cJSON_CreateNumber(ModbusSolisRegisters->familyLoadPower);
    if (Node)
      cJSON_AddItemToObject(SolarData, "familyLoadPower", Node);
    Node = cJSON_CreateString("kW");
    if (Node)
      cJSON_AddItemToObject(SolarData, "familyLoadPowerStr", Node);
  }

  // the outer pieces
  Node = cJSON_CreateString("success");
  if (Node)
    cJSON_AddItemToObject(SolarJson, "msg", Node);
  Node = cJSON_CreateBool(true);
  if (Node)
    cJSON_AddItemToObject(SolarJson, "success", Node);

  // generate return string
  Ret = cJSON_Print(SolarJson);
  cJSON_Delete(SolarJson);
  return Ret;
}

// create TCP server for operating as a modbus slave/forwarder
static SOCKET CreateModbusTCPServer(void)
{
  struct sockaddr_in ServerAddr; 
  SOCKET Sfd;
  socklen_t SLen;

  memset(&ServerAddr, 0, sizeof(ServerAddr));

  ServerAddr.sin_family = AF_INET;
  ServerAddr.sin_port = htons(502);  // default modbus TCP port
  ServerAddr.sin_addr.s_addr = INADDR_ANY;

  Sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (Sfd < 0)
  {
    perror("socket: ");
    return Sfd;
  }

  SLen = sizeof(ServerAddr);
  if (bind(Sfd, (struct sockaddr*)&ServerAddr, SLen) < 0)
  {
    perror("bind: ");
    closesocket(Sfd);
    return -1;
  }
  if (listen(Sfd, MAX_MODBUS_TCP_CLIENTS) < 0)
  {
    perror("listen: ");
    closesocket(Sfd);
    return -1;
  }

  return Sfd;
}

// thread used to service modbus TCP clients
static void ModbusTcpServiceThread(SOCKET ServerSfd)
{
  fd_set FdSet;
  int MaxSfd = ServerSfd;

  // got no server, bail
  if (ServerSfd < 0)
    return;

  while (1)
  {
    // add server to the select list
    FD_ZERO(&FdSet);
    FD_SET(ServerSfd, &FdSet);

    // add connected clients to the polling list
    for (uint32_t i = 0; i < MAX_MODBUS_TCP_CLIENTS; i++)
    {
      if (ModbusTCPClients[i] )
      {
        FD_SET(ModbusTCPClients[i], &FdSet);
        if (ModbusTCPClients[i] > MaxSfd)
          MaxSfd = ModbusTCPClients[i];
      }
    }

    // see if any require our attention
    int Rc = select(MaxSfd + 1, &FdSet, nullptr, nullptr, nullptr);
    if (Rc > 0)
    {
      // new client connection?
      if (FD_ISSET(ServerSfd, &FdSet))
      {
        // accept the connection
        SOCKET Sfd = accept(ServerSfd, nullptr, 0);

        if (Verbose)
          printf("New client connection\n");

        if (Sfd < 0)
          perror("accept:");
        else
        {
          // find a free slot in the list of client connections
          // and add it
          bool AcceptClient = false;

          for (uint32_t i = 0; i < MAX_MODBUS_TCP_CLIENTS; i++)
          {
            if (!ModbusTCPClients[i] )
            {
              ModbusTCPClients[i] = Sfd;
              AcceptClient = true;
              break;
            }
          }
          if (!AcceptClient)
          {
            printf("Max no of client connections exceeded\n");
            closesocket(Sfd);
          }
          else
          {
            // disable Naggle algorithm so the response packets get sent out immediately
            const int Disable = 1;
            setsockopt(Sfd, IPPROTO_TCP, TCP_NODELAY, (const char*)&Disable, sizeof(int));
          }
        }
      }

      // any clients require attention?
      for (uint32_t i = 0; i < MAX_MODBUS_TCP_CLIENTS; i++)
      {
        if (ModbusTCPClients[i] >= 0)
        {
          if (FD_ISSET(ModbusTCPClients[i], &FdSet))
          {
            // service the request
            if (!ServiceModbusTcpClient(ModbusTCPClients[i]))
            {
              closesocket(ModbusTCPClients[i]);
              ModbusTCPClients[i] = 0;
            }
          }
        }
      }
    }
    else
    {
      perror("select: ");
      break;
    }
  }
}

// service next pending Modbus TCP client request
static bool ServiceModbusTcpClient(SOCKET Sfd)
{
  uint8_t Frame[1400];
  bool ValidClient = false;

  // read the request
  auto Bytes = recv(Sfd, (char*)Frame, sizeof(Frame), 0);
  if (Bytes < 0)
    perror("ServiceModbusTcpClient: recv: ");
  else if (!Bytes)
  {
    if (Verbose)
      printf("Client has closed connection\n");
  }
  else
    ValidClient = true;

  // nothing to do 
  if (!ValidClient)
    return false;

  // try and construct an ADU object from this frame
  // since TCP is stream oriented, in theory 2 things could go wrong here....
  // - we could read insufficient data for a complete frame. This is unlikely given how small they are
  // - we could read > 1 frame, this shouldn't happen because the client should wait for us to respond to 
  //   the last request before sending another. 
  // either way, if those do crop up, we just ignore them & let the client retry
  ModbusTcpAdu *Adu = new ModbusTcpAdu(Sfd, Frame, Bytes);
  if (Adu->IsValidFrame())
  {
    bool AddNewRequest = true;
    bool Transacted = false;

    // lockout updates to the request list for the duration
    boost::lock_guard<boost::mutex> ClientListLock(ModbusClientMutex);

    // check to see if we've already got an identical request pending
    for (auto It = ModbusClientRequests.begin(); It != ModbusClientRequests.end(); ++It)
    {
      if ((*It)->IsIdenticalAdu(*Adu))
      {
        auto ExistingAdu = (*It);

        // update the existing transaction write data with new
        // - if this differs from the existing, will adjust the processed flag accordingly
        if (Adu->IsWriteTransaction())
          ExistingAdu->UpdateRegisterData(Adu->GetRegisterData());

        // we have, has it been processed?
        if (ExistingAdu->IsProcessed())
        {
          // yes, send it to the client using the transaction id of this one
          if (!ExistingAdu->TcpSendResponse(Sfd, Adu->GetTransactionId()))
            ValidClient = false;
          Transacted = true;
        }

        // no need to add this one to the list
        AddNewRequest = false;
        break;
      }
    }

    // if request didn't get answered, send a "server busy" response back
    if (!Transacted)
    {
      if (!Adu->TcpSendDeviceBusy())
        ValidClient = false;
    }

    // need to add this to the list of pending requests?
    if (AddNewRequest && ValidClient)
    {
      if (Verbose)
        printf("Add new transaction: %s\n", Adu->GetTransactionString().c_str());
      ModbusClientRequests.push_back(Adu);
    }
    else
      delete Adu;  // request already exists in queue so discard
  }
  else // failed to construct a frame, just return
    delete Adu;

  return ValidClient;
}

// process any pending TCP modbus requests from an external client
bool ProcessPendingModbusTcpRequests(const char *Device)
{
  uint32_t TransactCount = 0u;
  // maxm. no of transactions/register accesses allowed in this slot
  // our "normal" poll reads 9 registers in 4 distinct transactions which leaves
  // 6s to spare in the final cycle. The idea is that these custom transactions
  // (if >1) should more or less take the same time so we don't disrupt the overall
  // timing. 
  // Given the logger can read 250 registers in ~3s, this should be very conservative...

  const uint32_t MaxTransactions = 25;  

  {
    boost::lock_guard<boost::mutex> ClientListLock(ModbusClientMutex);

    // check for and remove any stale read data to force a refresh
    auto StaleReads = std::remove_if(ModbusClientRequests.begin(), ModbusClientRequests.end(),
      [](ModbusTcpAdu *Inst) {
      if (Inst->IsStale())
      {
        if (Verbose) printf("Deleting stale: %s\n", Inst->GetTransactionString().c_str());
        delete Inst;
        return true;
      }
      else
        return false;
    });

    ModbusClientRequests.erase(StaleReads, ModbusClientRequests.end());
  }

  while (true)
  {
    ModbusTcpAdu *PendingRequest = nullptr;

    {
      uint32_t Unprocessed = 0;

      // lockout access to the client list whilst we find a pending request
      boost::lock_guard<boost::mutex> ClientListLock(ModbusClientMutex);

      // check to see if we've already got any request pending
      for (auto It = ModbusClientRequests.begin(); It != ModbusClientRequests.end(); ++It)
      {
        if (!(*It)->IsProcessed())
        {
          if (!PendingRequest)
            PendingRequest = *It;
          Unprocessed++;
        }
      }

      if (Verbose)
      {
        printf("ModbusClientRequests total: %u\n", ModbusClientRequests.size());
        printf("ModbusClientRequests unprocessed: %u\n", Unprocessed);
      }
    }

    // nothing pending
    if (!PendingRequest)
      return TransactCount ? true : false;

    // if already done at least one transaction, check this one won't tip us over the max for this slot
    // - if so, bail now 
    if (TransactCount && ((TransactCount + PendingRequest->GetRegisterCount()) > MaxTransactions))
      return true;

    if (Verbose)
      printf("Executing: %s\n", PendingRequest->GetTransactionString().c_str());

    // track total transactions
    TransactCount += PendingRequest->GetRegisterCount();

    // perform the request. Note that this since this can be time consuming, the client request
    // lock is deliberately released during this period. This also means that the TCP service
    // thread must not remove/delete anything in the list - which it doesn't
    if (!PendingRequest->PerformRTUTransaction(Device))
    {
      // if the request failed, just remove it from the list. The client can retry again if it wants
      boost::lock_guard<boost::mutex> ClientListLock(ModbusClientMutex);
      for (auto It = ModbusClientRequests.begin(); It != ModbusClientRequests.end(); ++It)
      {
        if ((*It) == PendingRequest)
        {
          ModbusClientRequests.erase(It);
          break;
        }
      }
      delete PendingRequest;
    }
    else if (PendingRequest->IsWriteTransaction())
    {
      boost::lock_guard<boost::mutex> ClientListLock(ModbusClientMutex);

      // if just performed a write transaction, remove any transactions that have overlapping
      // register regions as they will now be invalid
      auto MatchingRequests = std::remove_if(ModbusClientRequests.begin(), ModbusClientRequests.end(),
        [PendingRequest](ModbusTcpAdu *Inst) {
        if ((Inst != PendingRequest) && Inst->InvalidateAdu(*PendingRequest))
        {
          if (Verbose) printf("Deleting invalidated: %s\n", Inst->GetTransactionString().c_str());
          delete Inst;
          return true;
        }
        else
          return false;
      });

      ModbusClientRequests.erase(MatchingRequests, ModbusClientRequests.end());
    }
  }

  // can never reach here now
  return true;
}

int main(int argc, char *argv[])
{
  ModbusSolisRegister_t ModbusSolisRegisters;
  uint8_t SlaveId = 1;
  // should have 50s worth of free time, which allows for 3 requests at 20s intervals
  const uint32_t RequestsPerCycle = 3;
  const uint32_t PollDelay = 18;
  char *jSon;
  SOCKET sFd, ModbusTcpServerFd ;
  int EnBroadcast = 1 ;
  struct sockaddr_in BroadcastAddr ;
#ifdef WIN32
  WORD wVersionRequested;
  WSADATA wsaData;
  int Err;

  // Win32 socket guff
  wVersionRequested = MAKEWORD(2, 0);

  Err = WSAStartup(wVersionRequested, &wsaData);
  if (Err != 0)
  {
    perror("WSAStartup");
    return -1;
  }

  if (LOBYTE(wsaData.wVersion) != 2 ||
    HIBYTE(wsaData.wVersion) != 0)
  {
    printf("Unsupported socket socket lib\n");
    WSACleanup();
    return -1;
  }

#endif

  if (argc < 2)
  {
    printf("Usage: modbus-solis-broadcast <input> [verbose=0] [slaveid=1]\n");
    return -1;
  }

  if (argc > 2)
    Verbose = strtoul(argv[2], NULL, 0) ? true : false ;

  if (argc > 3)
    SlaveId = strtoul(argv[3],NULL,0) ;

  // setup broadcast socket for sending out the data to clients
  sFd = socket(AF_INET,SOCK_DGRAM,0) ;
  if ( sFd < 0 )
  {
    perror("socket") ;
    return -1 ;
  }
  if ( setsockopt(sFd, SOL_SOCKET, SO_BROADCAST, (char*)&EnBroadcast, sizeof(EnBroadcast)) < 0 )
  {
    perror("setsockopt - broadcast") ;
    return -1 ;
  }
  memset((void*)&BroadcastAddr, 0, sizeof(struct sockaddr_in));
  BroadcastAddr.sin_family = AF_INET;
  BroadcastAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
  
  // create Modbus TCP server for handling remote requests
  ModbusTcpServerFd = CreateModbusTCPServer() ;
  if (ModbusTcpServerFd >= 0)
  {
    // start thread responsible for servicing modbus TCP clients
    boost::thread ServiceThread(ModbusTcpServiceThread,ModbusTcpServerFd);
  }

  printf( "Starting poll\n") ;
#ifndef WIN32
  // sync to the next access performed by the data logger
  while (SyncWithLogger(argv[1]))
#else
  while (1)
#endif
  {
    // should have 50s worth of free time, which allows for 3 requests at 20s intervals
    for (uint32_t i = 0; i < RequestsPerCycle; i++)
    {
      // if there is at least one client requiring attention, use this cycle to
      // service it rather than our usual poll
      if (ProcessPendingModbusTcpRequests(argv[1]))
      {
      }
      else if ( ModBusReadSolisRegisters(argv[1], &ModbusSolisRegisters, SlaveId))
      {
        if (Verbose)
        {
          printf("Battery capacity SOC: %u%%\n", ModbusSolisRegisters.batteryCapacitySoc);
          printf("Battery power: %f kW\n", ModbusSolisRegisters.batteryPower);
          printf("House load power: %f kW\n", ModbusSolisRegisters.familyLoadPower);
          printf("Current Generation - DC power o/p: %f kW\n", ModbusSolisRegisters.pac);
          printf("Meter total active power: %f kW\n", ModbusSolisRegisters.psum);
          printf("Inverter power generation today: %f kW\n", ModbusSolisRegisters.etoday);
        }

        // generate the JSON data, aligned to the Solis API
        jSon = GenerateJson(&ModbusSolisRegisters);
        if (jSon)
        {
          if ( Verbose )
            printf("JSON data: %s:\n", jSon);

          // send out to clients
          BroadcastAddr.sin_port = htons(52005);
          //if (sendto(sFd, jSon, strlen(jSon), 0, (struct sockaddr*) &BroadcastAddr, sizeof(struct sockaddr_in)) < 0)
          //  perror("sendto");
          free(jSon);
        }
        else
          printf("Failed to generate JSON data\n");
      }
      else
      {
        printf("Failed to retrieve modbus data from inverter\n");
      }


      // don't sleep on the last cycle
      if (i < (RequestsPerCycle - 1))
      {
#ifndef WIN32
        // open serial port to monitor for traffic while we sleep
        int Fd = open(argv[1], O_RDONLY | O_NONBLOCK);
        uint8_t ScratchBuf[256];
        bool ContinuePoll = false;

        if (Fd < 0)
        {
          perror("open serial");
          break;
        }

        sleep(PollDelay);

        // poll for any data arrived in the meantime, under normal circumstances, it shoudn't
        // EXCEPT when the logger performs it's daily reset...
        auto Rc = read(Fd, ScratchBuf, sizeof(ScratchBuf));
        if (Rc < 0)
        {
          // no data read, verify it's because there's none there and if so, continue with our next request
          if (errno != EAGAIN)
            perror("read");
          else
            ContinuePoll = true;
        }
        else if (Verbose)
        {
          printf("Detected serial data, forcing re-sync\n") ;
        }
        close(Fd);
        if ( !ContinuePoll )
          break;
#else
        Sleep(PollDelay*1000);
#endif
      }
    }
  }

  closesocket(sFd);
  return 0;
}
