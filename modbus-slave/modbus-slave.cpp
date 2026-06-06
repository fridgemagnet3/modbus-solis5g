#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#ifdef WIN32
#include <io.h>
#pragma warning(disable : 4996)
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#define Sleep(a) usleep(a*1000)
#endif
#include "registers.h"
#include "read_transact.h"
#include "write_transact.h"
#include <boost/chrono/chrono.hpp>
#include <boost/date_time.hpp>
#include <modbus/modbus.h>
#include <iostream>
#include <inttypes.h>

// modebus-slave. This is designed to loosely emulate the behaviour of the Solis inverter
// when connected to the wifi logger
//

static int32_t ModBusHandleResponse(const char *Device, uint8_t Slave, modbus_mapping_t *ModBusMapping, uint32_t LoggerInterval = 60u)
{
  using namespace boost::chrono;
  modbus_t *Ctx = modbus_new_rtu(Device, 9600, 'N', 8, 1);
  uint8_t *Request;
  int Rc;
  uint32_t Timeout = LoggerInterval;
  bool Poll = true;
  uint32_t Sec, uSec ;

  if (!Ctx)
  {
    printf("modbus_new_rtu: %s\n", modbus_strerror(errno));
    return -1 ;
  }

#ifndef WIN32
  // configure the link to use RTS, for a USB connected device this (I don't think)
  // will actually do anything but it does cause the RTS delay to be honoured which is
  // required for a RS485 link in order to allow time for the tranceivers to be turned off/on
  if ( modbus_rtu_set_rts ( Ctx, MODBUS_RTU_RTS_UP ) < 0 )
  {
    printf("modbus_rtu_set_serial_mode: %s\n", modbus_strerror(errno));
    modbus_free(Ctx);
    return -1;
  }

  // set the RTS delay, this is the delay from receiving the request to responding to it
  // by default (if RTS mode is unset), there is no delay, which is fine for RS232 but not for
  // 485 where we need to allow time for the transceivers to turn off/on
  // Based on traffic from modbus-sniffer, the Solis inverter typically takes anywhere 
  // between ~35ms to 100ms to respond so we err on the conversative side here
  if ( modbus_rtu_set_rts_delay(Ctx, 30*1000 ) == 0 )
    printf( "RTS delay: %d\n", modbus_rtu_get_rts_delay(Ctx)) ;
  else
  {
    printf("modbus_rtu_set_rts_delay: %s\n", modbus_strerror(errno));
    modbus_free(Ctx);
    return -1;
  }

  // set timeout for receiving individual bytes. In my setup, when the ESP32 turns OFF
  // it's transmitters, it generates framing errors (similar to behaviour seen with the
  // inveter/logger although it's unclear whether this is turning them on/off). Given this
  // happens at the END of the request and there are now delays applied on the ESP side
  // (this I think is also required to some degree in order to adhere to the Modbus spec)
  // When those stray bytes are received, they should trigger the timeout condition,
  // resulting the the data then being discarded. Operationally, you'll see this 
  // generating a 'Connection timed out' message between every request.
  // What this won't deal with is any noise arising from when the transmitters are turned ON

  // 40ms is also a bit trial/error based on the behaviour of the ESP32, which has an 80ms delay.
  // dropping it further can result in timeouts being triggered mid transaction - all of which may be
  // due to the accuracy of the timers used by each device
  if ( modbus_set_byte_timeout ( Ctx, 0, 40*1000 ) == 0 )
  {
    if ( modbus_get_byte_timeout ( Ctx, &Sec, &uSec ) == 0 )
      printf( "Byte timeout: %u:%u\n", Sec, uSec ) ;
    else
      printf("modbus_get_byte_timeout: %s\n", modbus_strerror(errno));
  }
  else
  {
    printf("modbus_get_byte_timeout: %s\n", modbus_strerror(errno));
    modbus_free(Ctx);
    return -1;
  }
#endif

  modbus_set_debug(Ctx, 1);

  if (modbus_set_slave(Ctx, Slave) == -1)
  {
    printf("modbus_set_slave: %s\n", modbus_strerror(errno));
    modbus_close(Ctx);
    modbus_free(Ctx);
    return -1;
  }

  if (modbus_connect(Ctx) == -1)
  {
    printf("modbus_connect: %s\n", modbus_strerror(errno));
    modbus_free(Ctx);
    return -1;
  }

  Request = new uint8_t[MODBUS_RTU_MAX_ADU_LENGTH];

  steady_clock::time_point TransactTime(steady_clock::now());

  // continue to service requests until it's time to simulate the next logger transaction - every 60s
  while (Poll)
  {
    steady_clock::time_point Now(steady_clock::now());

    uint32_t Elapsed = (uint32_t)(duration_cast<seconds>(Now - TransactTime).count());

    // adjust timeout accordingly
    if (Elapsed < LoggerInterval)
    {
      Timeout = LoggerInterval - Elapsed;
      modbus_set_indication_timeout(Ctx, Timeout, 0);
      printf("Set timeout to %u\n", Timeout);

      do
      {
        Rc = modbus_receive(Ctx, Request);
      } while (Rc == 0);

      if (Rc < 0)
      {
        printf("modbus_receive: %s\n", modbus_strerror(errno));
      }
      else
      {
        printf("Request received Rc= %d\n", Rc);

        if (modbus_reply(Ctx, Request, Rc, ModBusMapping) < 0)
        {
          printf("modbus_reply: %s\n", modbus_strerror(errno));
        }
      }
    }
    else
      Poll = false;
  }

  modbus_close(Ctx);
  modbus_free(Ctx);

  delete Request;

  return 0;
}

#ifdef WIN32

static HANDLE OpenW32Serial(const char *Device, int Flags)
{
  DWORD CommFlags = 0;

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

  return hComm;
}

static int OpenW32SerialAsFd(const char *Device, int Flags)
{
  HANDLE hComm = OpenW32Serial(Device, Flags);
  if (hComm != INVALID_HANDLE_VALUE)
    return _open_osfhandle((intptr_t)hComm, Flags);
  else
    return -1;
}

#endif

static void TransactSlave(modbus_t *Ctx)
{
  int Rc;
  const int RegCount = 4;
  int Registers[] = { 35000, 36000, 2999, 33000 };
  uint16_t RegValue;

  // logger has a 3 second timeout on each request (except the final one)
  modbus_set_response_timeout(Ctx, 3, 0);

  for (int i = 0; i < RegCount; i++)
  {
    printf("Read register %d\n", Registers[i]);
    Rc = modbus_read_input_registers(Ctx, Registers[i], sizeof(uint16_t), &RegValue);
    if (Rc == sizeof(uint16_t))
    {
      printf("Reg Value: %hx\n", RegValue);
    }
    else
      printf("modbus_read_input_registers: %s\n", modbus_strerror(errno));

    if (i == (RegCount-1))
      modbus_set_response_timeout(Ctx, 1, 0); // set to 1s timeout on final tx, the 3s inter-slave delay then results in a 4s timeout if nothing responds
  }
}

// simulate a wifi logger transaction, this just dumps out representative, fixed data
static bool SimulateBusTransaction(const char *Device, uint32_t &Elapsed)
{
  int Fd ;
  bool Status = true;

#ifndef WIN32
  Fd = open(Device, O_WRONLY);
#else
  Fd = OpenW32SerialAsFd(Device, O_WRONLY );
#endif

  if (Fd < 0)
  {
    perror("Failed to open input");
    return false;
  }

  boost::posix_time::ptime RequestTime(boost::posix_time::second_clock::local_time());
  std::cout << std::endl << "Simulated logger transact at " << boost::posix_time::to_simple_string(RequestTime) << "..." << std::endl;

  // Primarily this is about simulating the timing behaviour. There are 13 distinct
  // Modbus transactions performed occurring at anywhere from 136ms thru 242ms apart
  // overall the time taken is around 7s
  const uint32_t TransactCount = 13u ;
  const uint32_t TransactSize = __read_transact_bin_len / TransactCount ;
  uint8_t *Ptr = __read_transact_bin ;
  const uint32_t TransactDelay = 600 ; 

  printf("Performing logger read register transactions\n");
  for(uint32_t i=0 ; i < TransactCount ; i++ )
  {
    if (write(Fd, Ptr, TransactSize) < 0)
    {
      perror("write");
      Status = false;
    }
    Ptr+=TransactSize ;
    Sleep(TransactDelay) ;
  }

  close(Fd);

  if (!Status)
    return false;

  // should be about 8s elapsed

  {
    boost::posix_time::ptime EndTime(boost::posix_time::second_clock::local_time());
    boost::posix_time::time_duration ElapsedTime = EndTime - RequestTime;
    Elapsed = ElapsedTime.total_seconds();
    printf("Elapsed: %02" PRId64 ":%02" PRId64 ".%03" PRId64"\n", ElapsedTime.minutes(), ElapsedTime.seconds(), ElapsedTime.fractional_seconds());
  }


  // setup for starting to poll slaves
  const uint32_t MaxSlaves = 10;
  const uint32_t InterSlaveDelay = 3000;  // there is always a 3s delay between each set of slave transactions

  modbus_t *Ctx = modbus_new_rtu(Device, 9600, 'N', 8, 1);

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

  modbus_set_debug(Ctx, 1);

  // transact the slaves
  for (uint32_t Slave = 2; Slave <= MaxSlaves; Slave++)
  {
    if (modbus_set_slave(Ctx, Slave) == -1)
    {
      printf("Error setting slave %u: %s\n", Slave, modbus_strerror(errno));
    }
    else
    {
      printf("\nTransact Slave %u\n", Slave);
      TransactSlave(Ctx);
    }
    Sleep(InterSlaveDelay);
  }
  modbus_close(Ctx);
  modbus_free(Ctx);

  boost::posix_time::ptime EndTime(boost::posix_time::second_clock::local_time());
  boost::posix_time::time_duration ElapsedTime = EndTime - RequestTime ;

  // for non-responsive slaves, this should be ~2min, 5 seconds
  // for responsive slaves, should be ~40s
  Elapsed = ElapsedTime.total_seconds() ;
  printf("Elapsed: %02" PRId64 ":%02" PRId64 ".%03" PRId64"\n", ElapsedTime.minutes(), ElapsedTime.seconds(), ElapsedTime.fractional_seconds());

  return Status;
}

int main(int argc, char *argv[])
{
  using namespace boost::chrono;
  uint8_t Slave = 1u;
  modbus_mapping_t *ModBusMapping;
  int i;
  int32_t Rc = 1 ;
  uint16_t *RegPtr = (uint16_t*)registers_bin;
  bool SimulateLogger = true;
  uint32_t Elapsed = 0 ;

  if (argc < 2)
  {
    printf("Usage: modbus-slave <input> [slave address=1] [simulate-datalogger=1]\n");
    return -1;
  }

  if ( argc > 2)
    Slave = (uint8_t)strtoul(argv[2], NULL, 0);

  if (argc > 3)
    SimulateLogger = strtoul(argv[3], NULL, 0) ? true : false;

  ModBusMapping = modbus_mapping_new_start_address(500, 1, 12500, 100, 43000, 1000, 33000, 2500);
  if (!ModBusMapping)
  {
    printf("modbus_mapping_new_start_address: %s\n", modbus_strerror(errno));
    return -1;
  }

  // The contents of 'registers.h' proivdes a complete snapshot of the registers
  // generated by sniffing the modbus between the inverter and datalogger.
  // These are loaded into the register block which the slave will respond with.

  // Pertinent values:

  // 33057:33058: Current Generation - DC power o/p: 240 W
  // 33135: Batttery charge status(0=charge,1=discharge): 1
  // 33139: Battery capacity SOC : 15 %
  // 33147: House load power: 389 W
  // 33149:33150: Battery power: 178 W
  // 33263:33264 : Meter total active power : -0.176000 kW
  for (i = 0; i < registers_bin_len/sizeof(uint16_t); i++)
    ModBusMapping->tab_input_registers[i] = *RegPtr++;

  // set a dummy, non-zero bit count for testing
  ModBusMapping->tab_input_bits[0] = 0x55 ;
  
  // start the run
  while (Rc>=0)
  {
    if (SimulateLogger)
      SimulateBusTransaction(argv[1],Elapsed);
    // each logger cycle is performed every 5 minutes so we have whatever is left to issue requests
    Rc = ModBusHandleResponse(argv[1], Slave, ModBusMapping,300-Elapsed);
  }

  modbus_mapping_free(ModBusMapping);

  return 0;
}

