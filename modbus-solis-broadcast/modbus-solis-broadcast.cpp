#include <stdio.h>
#include <fcntl.h>
#ifndef WIN32
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <errno.h>
#include <termios.h>
#include <sys/socket.h>
#include <arpa/inet.h>
typedef int SOCKET;
#define closesocket close
#else
#include <io.h>
#pragma warning(disable : 4996)
#define sleep(x) Sleep(x*1000)
#endif
#include <errno.h>
#include <modbus/modbus.h>
#include <boost/date_time.hpp>
#include <boost/chrono/chrono.hpp>
#include <iostream>
#include <sstream>
#include <cjson/cJSON.h>

//
// Collect power generation data from Solis inverter via modbus, then
// encapsulate it as a limited JSON data packet using the same convention as
// the Solis cloud API, compress and UDP broadcast it out to interested clients
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

// read the required registers from modbus
static bool ModBusReadSolisRegisters(const char *Device, ModbusSolisRegister_t *ModbusSolisRegisters, uint8_t Slave = 1)
{
  using namespace boost::posix_time;
  modbus_t *Ctx = modbus_new_rtu(Device,9600,'N',8,1) ;
  uint16_t RegBank[16];
  int Rc = 0 ;
  bool Ret = false;

  if (Verbose)
  {
    ptime RequestTime(second_clock::local_time());

    std::cout << std::endl << "Issuing request at " << to_simple_string(RequestTime) << "..." << std::endl;
  }

  memset(ModbusSolisRegisters,0,sizeof(ModbusSolisRegister_t)) ;
  
  if (!Ctx)
  {
    printf("modbus_new_rtu: %s\n", modbus_strerror(errno));
    return false;
  }
  if (modbus_connect(Ctx) == -1)
  {
    printf("modbus_connect: %s\n", modbus_strerror(errno));
    modbus_free(Ctx);
    return false;
  }
  if (modbus_set_slave(Ctx, Slave) == -1)
  {
    printf("modbus_set_slave: %s\n", modbus_strerror(errno));
    modbus_close(Ctx);
    modbus_free(Ctx);
    return false;
  }
#ifdef WIN32
  // for testing
  modbus_set_response_timeout(Ctx, 15, 0);
  modbus_set_debug(Ctx, 1);
#endif

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
      printf("Timed out waiting for traffic, retrying...\n");
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
          printf("Got data %u bytes within %u seconds, re-syncing just to be sure...\n", Bytes, SyncTimeout-TimeOut.tv_sec);
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

    // wait for 10s of inactivity
    TimeOut.tv_sec = 10;
    TimeOut.tv_usec = 0;
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

int main(int argc, char *argv[])
{
  ModbusSolisRegister_t ModbusSolisRegisters;
  uint8_t SlaveId = 1;
  // should have 50s worth of free time, which allows for 3 requests at 20s intervals
  const uint32_t RequestsPerCycle = 3;
  const uint32_t PollDelay = 20;
  char *jSon;
  SOCKET sFd ;
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
  
  printf( "Starting poll\n") ;
#ifndef WIN32
  // sync to the next access performed by the data logger
  while (SyncWithLogger(argv[1]))
#endif
  {
    // should have 50s worth of free time, which allows for 3 requests at 20s intervals
    for (uint32_t i = 0; i < RequestsPerCycle; i++)
    {
      if (ModBusReadSolisRegisters(argv[1], &ModbusSolisRegisters, SlaveId))
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
          if (sendto(sFd, jSon, strlen(jSon), 0, (struct sockaddr*) &BroadcastAddr, sizeof(struct sockaddr_in)) < 0)
            perror("sendto");
          free(jSon);
        }
        else
          printf("Failed to generate JSON data\n");
      }
      else
        printf("Failed to retrieve modbus data from inverter\n");

      // don't sleep on the last cycle
      if ( i < (RequestsPerCycle-1)) 
        sleep(PollDelay);
    }
  }

  closesocket(sFd);
  return 0;
}
