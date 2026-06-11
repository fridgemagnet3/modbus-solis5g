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
typedef int SOCKET;
#define closesocket close
#define Sleep(a) usleep(a*1000)
#else
#include <io.h>
#pragma warning(disable : 4996)
#define O_NONBLOCK 0x80000
typedef int socklen_t;
#endif
#include <errno.h>
#include <modbus/modbus.h>
#include <boost/date_time.hpp>
#include <boost/chrono/chrono.hpp>
#include <iostream>
#include <sstream>
#include <cjson/cJSON.h>
#include <boost/crc.hpp>
#include <vector>
#include <algorithm>
#include "modbus_tcp_adu.h"
#ifdef RPI
#include "rpi_gpio.h"
#endif

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
  double   etoday;  // generation today (kWh)
  uint32_t batteryTotalChargeEnergy; // battery total charge (kWh)
  uint32_t batteryTotalDischargeEnergy;  // battery total discharge (kWh)
  uint32_t gridPurchasedTotalEnergy; // grid imported total (kWh)
  uint32_t gridSellTotalEnergy; // grid exported total (kWh)
  uint32_t eTotal; // solar generation total (kWh)
} ModbusSolisRegister_t;

static const uint32_t LoggerCycleTime = 300u; // 5 minutes

static bool Verbose = false;

static uint32_t LoggerFail = 0u;

// array of connected modbus clients
static SOCKET ModbusTCPClients[MAX_MODBUS_TCP_CLIENTS];
// pending client requests
static std::vector<ModbusTcpAdu*> ModbusClientRequests;
// mutex used to lock access to the ModbusClientRequests vector
static boost::mutex ModbusClientMutex;

static bool ServiceModbusTcpClient(SOCKET Sfd);


// read the required registers from modbus
static bool ModBusReadSolisRegisters(const char *Device, ModbusSolisRegister_t *ModbusSolisRegisters, 
                                      uint32_t &Elapsed, uint8_t Slave = 1)
{
  using namespace boost::posix_time;
  modbus_t *Ctx = ModbusTcpAdu::CreateModbusRtuSession(Device, Slave);
  uint16_t RegBank[16];
  int Rc = 0 ;
  bool Ret = true;
  ptime RequestStart(microsec_clock::local_time());

  if (Verbose)
    std::cout << std::endl << "Issuing request at " << to_simple_string(RequestStart) << "..." << std::endl;
  Elapsed = 0u ;
  
  memset(ModbusSolisRegisters,0,sizeof(ModbusSolisRegister_t)) ;
  
  if (!Ctx)
    return false;

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
    ModbusSolisRegisters->batteryPower /= 1000.0; // convert to kW
    ModbusSolisRegisters->familyLoadPower = (double)RegBank[12] / 1000; // 33147
  }
  else
  {
    printf("modbus_read_input_registers: %s\n", modbus_strerror(errno));
    Ret = false;
  }

  if (Ret)
  {
    // 33057:33058: Current Generation
    Rc = modbus_read_input_registers(Ctx, 33057, sizeof(uint32_t) / sizeof(uint16_t), RegBank);
    if (Rc == sizeof(uint32_t) / sizeof(uint16_t))
    {
      uint32_t Generation = (RegBank[0] << 16) + RegBank[1];   // expressed in watts
      ModbusSolisRegisters->pac = (double)Generation / 1000;  // return as kW
    }
    else
    {
      printf("modbus_read_input_registers: %s\n", modbus_strerror(errno));
      Ret = false;
    }
  }

  if (Ret)
  {
    // 33263:33264: Meter total active power
    Rc = modbus_read_input_registers(Ctx, 33263, sizeof(int32_t) / sizeof(uint16_t), RegBank);
    if (Rc == sizeof(uint32_t) / sizeof(uint16_t))
    {
      int32_t ActivePower = MODBUS_GET_INT32_FROM_INT16(RegBank, 0);
      ModbusSolisRegisters->psum = (double)ActivePower * 0.001;
    }
    else
    {
      printf("modbus_read_input_registers: %s\n", modbus_strerror(errno));
      Ret = false;
    }
  }

  if (Ret)
  {
    const int NoRegisters = 7;

    // 33029-33030: Inverter total power generation
    // 33035:       Intverter power generation today
    Rc = modbus_read_input_registers(Ctx, 33029, NoRegisters, RegBank);
    if (Rc == NoRegisters)
    {
      // expressed in kWh
      ModbusSolisRegisters->eTotal = (RegBank[0] << 16) + RegBank[1];
      // expressed in 0.1kWh intervals
      ModbusSolisRegisters->etoday = (float)(RegBank[6])*0.1;
    }
    else
    {
      printf("modbus_read_input_registers: %s\n", modbus_strerror(errno));
      Ret = false;
    }
  }

  if (Ret)
  {
    const int NoRegisters = 14;

    // 33161:33162 - Battery charge total
    // 33165:33166 - Battery discharge total
    // 33169:33170 - Grid power imported total
    // 33173:33174 - Power exported from grid total
    Rc = modbus_read_input_registers(Ctx, 33161, NoRegisters, RegBank);
    if (Rc == NoRegisters)
    {
      // expressed in 1kWh intervals
      ModbusSolisRegisters->batteryTotalChargeEnergy = (RegBank[0] << 16) + RegBank[1];
      ModbusSolisRegisters->batteryTotalDischargeEnergy = (RegBank[4] << 16) + RegBank[5];
      ModbusSolisRegisters->gridPurchasedTotalEnergy = (RegBank[8] << 16) + RegBank[9];
      ModbusSolisRegisters->gridSellTotalEnergy = (RegBank[12] << 16) + RegBank[13];
    }
    else
    {
      printf("modbus_read_input_registers: %s\n", modbus_strerror(errno));
      Ret = false;
    }
  }


  modbus_close(Ctx);
  modbus_free(Ctx);

  ptime RequestEnd(microsec_clock::local_time());
  time_duration ElapsedTime = RequestEnd - RequestStart;
  Elapsed = ElapsedTime.total_milliseconds();
  
  return Ret;
}

// decode Modbus request and if not intended for our slave, respond with something
// that will (hopefully) persuade the logger to stop querying it
static int DecodeAndRespondToSlave(uint8_t *Buffer, uint32_t BufSz, uint8_t SlaveId, int SerialFd)
{
  const uint32_t MinMsgLen = 8;
  uint8_t ReqSlave;
  uint8_t FCodeReadInput = 4;
  uint16_t Crc;
  uint16_t Register;
  uint32_t MsgStart = 0 ;

  // discard any null bytes at the start of the message    
  while(BufSz)
  {
    if ( !Buffer[MsgStart] )
    {
      BufSz-- ;
      MsgStart++ ;
    }
    else
      break ;
  }

  if ( Verbose )
  {
    printf("DecodeAndRespondToSlave - size %u\nMsg: ",BufSz);
    for(uint32_t i=0 ; i < BufSz ; i++ )
      printf( "%02x ",Buffer[i]) ;
    printf("\n") ;
  }
  
  // messages we're expecting should all be single register read requests, 8 bytes long
  if (BufSz < MinMsgLen)
  {
    if (Verbose)
      printf("Message too short for a read input register function\n");
    return -1;
  }
  // check slave not us
  ReqSlave = Buffer[MsgStart];
  if (ReqSlave == SlaveId)
  {
    if (Verbose)
      printf("Message is for local inverter, ignoring\n");
    return SlaveId;
  }

  // check this is a read register request
  if (Buffer[MsgStart+1] != FCodeReadInput)
  {
    printf("Not a read input registers function, ignoring\n");
    return -1;
  }

  // compute then verify the CRC
  auto ModBusCrc = boost::crc_optimal<16, 0x8005, 0xFFFF, 0, true, true> {};
  ModBusCrc.process_bytes(&Buffer[MsgStart], MinMsgLen - sizeof(uint16_t));

  Crc = (Buffer[MsgStart+7] << 8) + Buffer[MsgStart+6];
  if (ModBusCrc.checksum() != Crc)
  {
    if (Verbose)
      printf("CRC Incorrect, should be %x\n", ModBusCrc.checksum());
    return -1;
  }

  // extract the register
  Register = (Buffer[MsgStart+2] << 8) + Buffer[MsgStart+3];

  if (Verbose)
    printf("Message for slave: %u, register: %u\n", ReqSlave, Register);

  // got what we need, now generate the response...
  uint8_t ResponseBuf[5];
  const uint8_t ExceptionIllegalData = 0x02;

  // initial attempt: respond with an illegal address exception
  ResponseBuf[0] = ReqSlave;
  ResponseBuf[1] = FCodeReadInput | 0x80;
  ResponseBuf[2] = ExceptionIllegalData;

  ModBusCrc.reset();
  ModBusCrc.process_bytes(ResponseBuf, 3);
  ResponseBuf[3] = ModBusCrc.checksum() & 0xff;
  ResponseBuf[4] = ModBusCrc.checksum() >> 8 ;

#ifdef RPI
  RTSHandler(nullptr,1) ;
#else
  Sleep(10);
#endif

  if ( write(SerialFd, ResponseBuf, sizeof(ResponseBuf) ) < 0 )
    printf("Error on serial write\n") ;

#ifndef WIN32
  // wait for serial data to drain
  tcdrain(SerialFd);
#endif

#ifdef RPI
  RTSHandler(nullptr,0) ;
#else
  // a delay may/may not be required here depending on how reliable 'tcdrain' actually is
  Sleep(30);
#endif

  return ReqSlave;
}

#ifdef WIN32
static HANDLE OpenW32Serial(const char *Device, int Flags)
{
  DWORD CommFlags = 0 ;

  if (Flags & O_WRONLY)
    CommFlags |= GENERIC_WRITE;
  else
    CommFlags = GENERIC_READ;
  if (Flags & O_RDWR)
    CommFlags |= (GENERIC_WRITE | GENERIC_READ);

  HANDLE hComm = CreateFile(Device, CommFlags, 0, NULL, OPEN_EXISTING, 0, NULL);
  if (hComm == INVALID_HANDLE_VALUE)
    return hComm;

  DCB Dcb;
  Dcb.DCBlength = sizeof(Dcb);

  if (!GetCommState(hComm, &Dcb))
  {
    CloseHandle(hComm);
    return INVALID_HANDLE_VALUE;
  }
  Dcb.BaudRate = CBR_9600;
  Dcb.ByteSize = 8;
  Dcb.StopBits = ONESTOPBIT;
  Dcb.Parity = NOPARITY;
  Dcb.fBinary = TRUE;
  Dcb.fOutxCtsFlow = FALSE;
  Dcb.fOutxDsrFlow = FALSE;
  Dcb.fDsrSensitivity = FALSE;
  Dcb.fTXContinueOnXoff = FALSE;
  Dcb.fOutX = FALSE;
  Dcb.fInX = FALSE;
  Dcb.fNull = FALSE;
  Dcb.fAbortOnError = FALSE;
  if (!SetCommState(hComm, &Dcb))
  {
    CloseHandle(hComm);
    return INVALID_HANDLE_VALUE;
  }

  COMMTIMEOUTS CTimeouts;

  CTimeouts.WriteTotalTimeoutMultiplier = 0;
  CTimeouts.WriteTotalTimeoutConstant = 0;
  if (Flags & O_NONBLOCK)
  {
    // emulates non-blocking behaviour
    CTimeouts.ReadIntervalTimeout = MAXDWORD;
    CTimeouts.ReadTotalTimeoutConstant = 0;
    CTimeouts.ReadTotalTimeoutMultiplier = 0;
  }
  else
  {
    CTimeouts.ReadIntervalTimeout = 200;
    CTimeouts.ReadTotalTimeoutMultiplier = 10; 
    CTimeouts.ReadTotalTimeoutConstant = 50;
  }
  if (!SetCommTimeouts(hComm, &CTimeouts))
  {
    CloseHandle(hComm);
    return INVALID_HANDLE_VALUE;
  }
  return hComm;
}

static int OpenW32SerialAsFd(const char *Device, int Flags)
{
  HANDLE hComm = OpenW32Serial(Device, Flags);
  if (hComm != INVALID_HANDLE_VALUE)
  {
    Flags &= ~O_NONBLOCK;
    return _open_osfhandle((intptr_t)hComm, Flags);
  }
  else
    return -1;
}

// Sync with the next transfer performed by the datalogger & wait for it to finish
// Windows version
static bool SyncWithLogger(const char *Device, uint8_t SlaveId, uint32_t &Elapsed)
{
  using namespace boost::posix_time;
  HANDLE hComm;
  COMMTIMEOUTS CTimeouts;
  bool BusIdle = false;
  uint8_t ScratchBuf[256];
  ptime SyncStart(microsec_clock::local_time());
  int BytesRead;
  int SerialFd;

  hComm = OpenW32Serial(Device, O_RDWR);
  if (hComm == INVALID_HANDLE_VALUE)
  {
    printf("Failed to open input: %d\n", GetLastError());
    return false ;
  }
  Sleep(10) ;
  PurgeComm(hComm, PURGE_RXCLEAR);

  if (!GetCommTimeouts(hComm, &CTimeouts))
  {
    printf("Failed to get comm timeouts: %d\n", GetLastError());
    CloseHandle(hComm);
    return false;
  }

  CTimeouts.ReadTotalTimeoutConstant = LoggerCycleTime*1000;
  if (!SetCommTimeouts(hComm, &CTimeouts))
  {
    printf("Failed to set comm timeouts: %d\n", GetLastError());
    CloseHandle(hComm);
    return false;
  }

  if ( Verbose )
    std::cout << std::endl << "Sync with logger at " << to_simple_string(SyncStart) << "..." << std::endl ;

  // create an associated file descriptor for parity with Linux version
  SerialFd = _open_osfhandle((intptr_t)hComm, O_RDWR);

  // first, wait for the next burst of traffic from the logger, this normally occurs every five minutes
  BytesRead = _read(SerialFd, ScratchBuf, sizeof(ScratchBuf));

  if (BytesRead>0)
  {
    ptime WaitIdle(microsec_clock::local_time());
    auto ElapsedTime = WaitIdle - SyncStart;

    if (Verbose)
    {
      std::cout << "Elapsed: " << ElapsedTime.total_seconds() << "s" << std::endl;
      std::cout << std::endl << "Wait for idle at " << to_simple_string(WaitIdle) << "..." << std::endl;
    }
    SyncStart = microsec_clock::local_time();

    DecodeAndRespondToSlave(ScratchBuf, BytesRead, SlaveId, SerialFd);

    // wait for ~8s of inactivity
    CTimeouts.ReadTotalTimeoutConstant = 8 * 1000;
    if (!SetCommTimeouts(hComm, &CTimeouts))
    {
      printf("Failed to set comm timeouts: %d\n", GetLastError());
      _close(SerialFd);
      return false;
    }

    // sit in loop waiting for the bus to become inactive again
    while (!BusIdle)
    {
      BytesRead = _read(SerialFd, ScratchBuf, sizeof(ScratchBuf));
      if (!BytesRead)
        BusIdle = true;
      else if ( BytesRead > 0 )
        DecodeAndRespondToSlave(ScratchBuf, BytesRead, SlaveId, SerialFd);
      else
      {
        printf("Error on read: %d\n", GetLastError());
        break;
      }
    }
  }
  else if ( !BytesRead )
  {
    if (Verbose)
      printf("Timed out waiting for traffic - going ahead anyway...\n");
    BusIdle = true;
    LoggerFail++;
  }
  else
  {
    printf("Error on read: %d\n", GetLastError());
  }
  _close(SerialFd);

  ptime SyncEnd(microsec_clock::local_time());
  time_duration ElapsedTime = SyncEnd - SyncStart;
  Elapsed = ElapsedTime.total_milliseconds();

  if (Verbose)
    std::cout << "Elapsed: " << ElapsedTime.total_seconds() << "s" << std::endl;

  return BusIdle;
}

#else
// Sync with the next transfer performed by the datalogger & wait for it to finish
// Linux version
static bool SyncWithLogger(const char *Device, uint8_t SlaveId,uint32_t &Elapsed)
{
  using namespace boost::posix_time;
  int Fd;
  fd_set FdSet;
  struct timeval TimeOut;
  bool BusIdle = true;
  int Rc ;
  uint8_t ScratchBuf[256] ;
  ptime SyncStart(microsec_clock::local_time()) ;
  struct termios Termios ;
  const uint32_t ReadInputRegReqSize = 8 ;
  static bool FirstRun = true ;
  bool Slave10Tx = false ;
  
  Fd = open(Device, O_RDWR);
  if (Fd < 0)
  {
    perror("Failed to open input");
    return false;
  }

  if ( tcgetattr(Fd,&Termios) < 0 )
  {
    perror("Failed to get terminal settings\n") ;
    close(Fd);
    return false ;
  }
  // set the minimum block size for a 'read' call which equates to the
  // size of a single read input registers request
  // 
  // the additional '2' allows for stray null bytes I get in the serial
  // stream, if you don't see that, then this can be removed
  Termios.c_cc[VMIN] = ReadInputRegReqSize + 2;
  // set the timeout interval before returning - 200ms which is about
  // the worst case between logger requests
  // 
  // This *should* ensure every read call gives us a single Modbus request
  // and possibly the response although we don't really care about those
  Termios.c_cc[VTIME] = 1 ;
  if ( tcsetattr(Fd,TCSANOW,&Termios) < 0 )
  {
    perror("Failed to set terminal settings\n") ;
    close(Fd);
    return false ;
  }

  if ( FirstRun )
  {
    FirstRun = false ;
    // USB devices don't flush properly so attempt to handle this
    // by delaying for a short while on first invocation
    if ( strstr(Device,"USB") )
      usleep(100*1000);
    tcflush(Fd,TCIOFLUSH);
  }
  
  FD_ZERO(&FdSet);
  FD_SET(Fd, &FdSet);

  if ( Verbose )
    std::cout << std::endl << "Sync with logger at " << to_simple_string(SyncStart) << "..." << std::endl ;

  while (BusIdle)
  {
    // first, wait for the next burst of traffic from the logger, this normally occurs every five minutes 
    TimeOut.tv_sec = LoggerCycleTime;
    TimeOut.tv_usec = 0;
    Rc = select(Fd + 1, &FdSet, NULL, NULL, &TimeOut);
    if (Rc == 0)
    {
      if (Verbose)
        printf("Timed out waiting for traffic - going ahead anyway...\n");
      LoggerFail++ ;
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
      BusIdle = false;

      if ( Verbose )
      {
        ptime WaitIdle(microsec_clock::local_time()) ;
        auto ElapsedTime = WaitIdle - SyncStart;
	    
        std::cout << "Elapsed: " << ElapsedTime.total_seconds() << "s" << std::endl;
        std::cout << std::endl << "Wait for idle at " << to_simple_string(WaitIdle) << "..." << std::endl ;
      }
	
      SyncStart = microsec_clock::local_time() ;
    }
  }

  // sit in loop waiting for the bus to become inactive again
  while (!BusIdle)
  {
    // read any pending data
    Rc = read(Fd, ScratchBuf, sizeof(ScratchBuf));

    if (Rc < 0)
    {
      perror("read");
      break;
    }
    else if ( Rc > 0 )
    {
      int ReqSlave = DecodeAndRespondToSlave(ScratchBuf, Rc, SlaveId, Fd);
      if ( ReqSlave == 10 )
        Slave10Tx = true ;
      else if ( ReqSlave == 2 ) // if there are multiple polls this cycle, make sure we reset the Tx flag
        Slave10Tx = false ;
    } 
    
    // this logic is designed to (in part) handle the logger reset behaviour...
    
    // Under normal conditions, it will poll all 10 slaves, then go idle for
    // the remaining 5 minute cycle. That means we just need
    // to wait for a short idle time (8s) before starting our transactions
    // However when it's come out of reset, it can often start further polls
    // much sooner. Under those conditions, it also never seems to complete all 
    // the slave polling, instead appears to bail, then restart. So we use this
    // behaviour to determine whether to hang around for longer - ie. if we
    // *never* see a slave 10 transaction, assume we're going through a reset 
    // and wait for much longer for the idle condition
    if (!Slave10Tx)
      TimeOut.tv_sec = 30 ;
    else
      TimeOut.tv_sec = 8;
    TimeOut.tv_usec = 0;
    Rc = select(Fd + 1, &FdSet, NULL, NULL, &TimeOut);
    if (Rc == 0)
      BusIdle = true;  // timed out, bus is now free
    else if (Rc < 0)
    {
      perror("select");
      break ;
    }
  }

  ptime SyncEnd(microsec_clock::local_time());
  time_duration ElapsedTime = SyncEnd - SyncStart;
  Elapsed = ElapsedTime.total_milliseconds();

  if (Verbose)
    std::cout << "Elapsed: " << ElapsedTime.total_seconds() << "s" << std::endl;

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
    Node = cJSON_CreateString("kWh");
    if (Node)
      cJSON_AddItemToObject(SolarData, "eTodayStr", Node);
    // eTotal - total solar generation
    Node = cJSON_CreateNumber(ModbusSolisRegisters->eTotal);
    if (Node)
      cJSON_AddItemToObject(SolarData, "eTotal", Node);
    Node = cJSON_CreateString("kWh");
    if (Node)
      cJSON_AddItemToObject(SolarData, "eTotalStr", Node);

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

    // battery charge/discharge
    Node = cJSON_CreateNumber(ModbusSolisRegisters->batteryTotalChargeEnergy);
    if (Node)
      cJSON_AddItemToObject(SolarData, "batteryTotalChargeEnergy", Node);
    Node = cJSON_CreateString("kWh");
    if (Node)
      cJSON_AddItemToObject(SolarData, "batteryTotalChargeEnergyStr", Node);

    Node = cJSON_CreateNumber(ModbusSolisRegisters->batteryTotalDischargeEnergy);
    if (Node)
      cJSON_AddItemToObject(SolarData, "batteryTotalDischargeEnergy", Node);
    Node = cJSON_CreateString("kWh");
    if (Node)
      cJSON_AddItemToObject(SolarData, "batteryTotalDischargeEnergyStr", Node);

    // grid today in/out
    Node = cJSON_CreateNumber(ModbusSolisRegisters->gridPurchasedTotalEnergy);
    if (Node)
      cJSON_AddItemToObject(SolarData, "gridPurchasedTotalEnergy", Node);
    Node = cJSON_CreateString("kWh");
    if (Node)
      cJSON_AddItemToObject(SolarData, "gridPurchasedTotalEnergyStr", Node);

    Node = cJSON_CreateNumber(ModbusSolisRegisters->gridSellTotalEnergy);
    if (Node)
      cJSON_AddItemToObject(SolarData, "gridSellTotalEnergy", Node);
    Node = cJSON_CreateString("kWh");
    if (Node)
      cJSON_AddItemToObject(SolarData, "gridSellTotalEnergyStr", Node);
  }

  // the outer pieces
  Node = cJSON_CreateString("success");
  if (Node)
    cJSON_AddItemToObject(SolarJson, "msg", Node);
  Node = cJSON_CreateBool(true);
  if (Node)
    cJSON_AddItemToObject(SolarJson, "success", Node);

  // this is non-standard but provides an indication of if (and how many times)
  // the logger has failed
  Node = cJSON_CreateNumber(LoggerFail);
  if (Node)
    cJSON_AddItemToObject(SolarJson, "loggerFail", Node);

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

  const int Enable = 1;
#ifdef WIN32
  setsockopt(Sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&Enable, sizeof(int));
#else
  setsockopt(Sfd, SOL_SOCKET, SO_REUSEADDR, (const void*)&Enable, sizeof(int));
#endif

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
      if (ModbusTCPClients[i])
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
            if (!ModbusTCPClients[i])
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

// process next pending modbus request from an external client
bool ProcessPendingModbusTcpRequest(const char *Device,uint32_t &Elapsed)
{
  ModbusTcpAdu *PendingRequest = nullptr;

  Elapsed = 0u;

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

    uint32_t Unprocessed = 0;

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
      printf("ModbusClientRequests total: %lu\n", ModbusClientRequests.size());
      printf("ModbusClientRequests unprocessed: %u\n", Unprocessed);
    }
  }

  // nothing pending
  if (!PendingRequest)
    return false;

  if (Verbose)
    printf("Executing: %s\n", PendingRequest->GetTransactionString().c_str());

  auto StartTransactTime = boost::chrono::high_resolution_clock::now();

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

  auto EndTransactTime = boost::chrono::high_resolution_clock::now();
  auto ElapsedTime = EndTransactTime - StartTransactTime;

  Elapsed = boost::chrono::duration_cast<boost::chrono::milliseconds>(ElapsedTime).count();

  return true;
}

int main(int argc, char *argv[])
{
  ModbusSolisRegister_t ModbusSolisRegisters;
  uint8_t SlaveId = 1;
  const uint32_t PollDelay = 16 * 1000u;  // 16 seconds
  const uint32_t LoggerCycleTimeMilliseconds = LoggerCycleTime * 1000u;
  const uint32_t PollThreshold = 5000u;  // 5 seconds
  uint32_t Elapsed;
  char *jSon;
  SOCKET sFd, ModbusTcpServerFd;
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


#ifdef RPI
  // Use BCM addressing for GPIO
#ifdef PI_MODEL_5 // used as a sense check for older versions of the library
  wiringPiSetupPinType(WPI_PIN_BCM) ;
#else
  wiringPiSetupGpio() ;
#endif
  // configure the GPIOs and set for receive only
#ifdef RS485_RE
  pinMode(RS485_RE,OUTPUT) ;
  digitalWrite(RS485_RE,LOW);
#endif
#ifdef RS485_DE
  pinMode(RS485_DE,OUTPUT) ;
  digitalWrite(RS485_DE,LOW);
#endif
#endif
  
  printf( "Starting poll\n") ;

  // sync to the next access performed by the data logger
  while (SyncWithLogger(argv[1],SlaveId,Elapsed))
  {
    uint32_t TimeToNextPoll;

    // work out how much time we have till the next logger poll is due
    if (Elapsed < LoggerCycleTimeMilliseconds)
    {
      const uint32_t MinElapsed = 50*1000 ;
      const uint32_t InterruptedMaxTimeToPoll = 150 * 1000;

      // handle the (most likely only at startup) condition where we happen to 
      // immediately detect traffic & therefore get a much shorter than expected elapsed time
      // If we detect *all* the logger traffic, Elapsed should be ~55s so allowing for a margin of
      // error, if less than that, then we've only picked up some of it. Worst case (assuming we've
      // just caught the tail end), the next poll will be about 2m55s later so again allowing for 
      // a margin of error, 150s (2.5mins) should be fine
      if ( Elapsed < MinElapsed )
        TimeToNextPoll = InterruptedMaxTimeToPoll;
      else
        TimeToNextPoll = LoggerCycleTimeMilliseconds - Elapsed;
    }
    else
      TimeToNextPoll = 1000u;  // if we didn't see any logger traffic, or was longer than expected
                               // just do the one transaction, then resync

    while (TimeToNextPoll)
    {
      if (Verbose)
        printf("Time to next poll: %u seconds\n", TimeToNextPoll/1000u);

      bool ServiceModbusTcp = false ;
      
      // prioritise servicing next pending TCP request (if any)
      if (ProcessPendingModbusTcpRequest(argv[1], Elapsed))
      {
        ServiceModbusTcp = true;
      }
      else if (ModBusReadSolisRegisters(argv[1], &ModbusSolisRegisters, Elapsed, SlaveId))
      {
        if (Verbose)
        {
          printf("Battery capacity SOC: %u%%\n", ModbusSolisRegisters.batteryCapacitySoc);
          printf("Battery power: %f kW\n", ModbusSolisRegisters.batteryPower);
          printf("House load power: %f kW\n", ModbusSolisRegisters.familyLoadPower);
          printf("Current Generation - DC power o/p: %f kW\n", ModbusSolisRegisters.pac);
          printf("Meter total active power: %f kW\n", ModbusSolisRegisters.psum);
          printf("Inverter power generation today: %f kW\n", ModbusSolisRegisters.etoday);
          printf("Battery total charge: %u kW\n", ModbusSolisRegisters.batteryTotalChargeEnergy);
          printf("Battery total discharge: %u kW\n", ModbusSolisRegisters.batteryTotalDischargeEnergy);
          printf("Grid power imported total: %u kW\n", ModbusSolisRegisters.gridPurchasedTotalEnergy);
          printf("Grid power exported total: %u kW\n", ModbusSolisRegisters.gridSellTotalEnergy);
          printf("Inverter total power generation: %u kW\n", ModbusSolisRegisters.eTotal);
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

        Elapsed += PollDelay;
      }
      else
      {
        printf("Failed to retrieve modbus data from inverter\n");
        Elapsed += PollDelay;
      }

      // update how much time we have left till the next poll
      if (TimeToNextPoll > Elapsed)
      {
        TimeToNextPoll -= Elapsed;
        // don't go down to the wire
        if (TimeToNextPoll < PollThreshold)
          TimeToNextPoll = 0u;
      }
      else
        TimeToNextPoll = 0u; 

      // don't sleep on the last cycle OR if we've just serviced a TCP request
      // basically keep going until there are none left
      if (!ServiceModbusTcp && TimeToNextPoll)
      {
        // open serial port to monitor for traffic while we sleep
#ifdef WIN32
        int Fd = OpenW32SerialAsFd(argv[1], O_RDONLY | O_NONBLOCK);
#else
        int Fd = open(argv[1], O_RDONLY | O_NONBLOCK);
#endif
        uint8_t ScratchBuf[256];
        bool ContinuePoll = false;

        if (Fd < 0)
        {
          perror("open serial");
          break;
        }

        Sleep(PollDelay);

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
        else if (Rc)
        {
          if (Verbose)
            printf("Detected serial data, forcing re-sync\n");
        }
        else  // on Windows, the byte count comes back as zero rather than an error
          ContinuePoll = true;

        close(Fd);
        if ( !ContinuePoll )
          break;
      }
    }
  }

  closesocket(sFd);
  closesocket(ModbusTcpServerFd);
  return 0;
}
