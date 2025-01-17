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
#endif
#include "registers.h"
#include "read_transact.h"
#include "write_transact.h"
#include <boost/chrono/chrono.hpp>
#include <boost/date_time.hpp>
#include <modbus/modbus.h>
#include <iostream>

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

// simulate a wifi logger transaction, this just dumps out representative, fixed data
static bool SimulateBusTransaction(const char *Device, uint32_t &Elapsed)
{
  static uint32_t Cycles = 0;
  int Fd ;
  bool Status = true;

#ifndef WIN32
  Fd = open(Device, O_WRONLY);
#else
  Fd = _open(Device, O_WRONLY | _O_BINARY);
#endif

  if (Fd < 0)
  {
    perror("Failed to open input");
    return false;
  }

  boost::posix_time::ptime RequestTime(boost::posix_time::second_clock::local_time());
  std::cout << std::endl << "Simulated logger transact at " << boost::posix_time::to_simple_string(RequestTime) << "..." << std::endl;

  printf("Performing logger write register transaction\n");

  // simulate the register write transaction performed by the logger
  if (write(Fd, __write_transact_bin, __write_transact_bin_len) < 0)
  {
    perror("write");
    Status = false;
  }

  // every 5 minutes, simulate the register read transaction performed by the logger
  if (!(Cycles % 5))
  {
    // Primarily this is about simulating the timing behaviour. There are 13 distinct
    // Modbus transactions performed occurring at anywhere from 136ms thru 242ms apart
    // overall the time taken is around 3s
    const uint32_t TransactCount = 13u ;
    const uint32_t TransactSize = __read_transact_bin_len / TransactCount ;
    uint8_t *Ptr = __read_transact_bin ;
    const useconds_t TransactDelay = 240*1000 ; // 

    printf("Performing logger read register transactions\n");
    for(uint32_t i=0 ; i < TransactCount ; i++ )
    {
      if (write(Fd, Ptr, TransactSize) < 0)
      {
        perror("write");
        Status = false;
      }
      Ptr+=TransactSize ;
      usleep(TransactDelay) ;
    }
  }
  Cycles++;

  close(Fd);

  boost::posix_time::ptime EndTime(boost::posix_time::second_clock::local_time());
  boost::posix_time::time_duration ElapsedTime = EndTime - RequestTime ;
  Elapsed = ElapsedTime.total_seconds() ;
  printf( "Elapsed: %u s\n", Elapsed) ;

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

  ModBusMapping = modbus_mapping_new_start_address(0, 0, 0, 0, 0, 0, 33000, 300);
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
  for (i = 0; i < ModBusMapping->nb_input_registers ; i++)
    ModBusMapping->tab_input_registers[i] = *RegPtr++;

  // start the run
  while (Rc>=0)
  {
    if (SimulateLogger)
      SimulateBusTransaction(argv[1],Elapsed);
    Rc = ModBusHandleResponse(argv[1], Slave, ModBusMapping,60-Elapsed);
  }

  modbus_mapping_free(ModBusMapping);

  return 0;
}

