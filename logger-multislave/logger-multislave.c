#include <stdio.h>
#include <stdint.h>
#include <modbus/modbus.h>
#ifndef WIN32
#include <unistd.h>
#include <errno.h>
#define Sleep(x) usleep(x*1000)
#endif

// Simulate logger behaviour querying multiple slaves on a cyclic basis

static void TransactSlave(modbus_t *Ctx);

int main(int argc, char *argv[])
{
  const uint32_t MaxSlaves = 10;
  const uint32_t InterSlaveDelay = 6000;  // 6s between each slave requests

  if (argc < 2)
  {
    printf("Usage: logger-multislave <device>\n");
    return -1;
  }

  modbus_t *Ctx = modbus_new_rtu(argv[1], 9600, 'N', 8, 1);

  if (!Ctx)
  {
    printf("modbus_new_rtu: %s\n", modbus_strerror(errno));
    return -1;
  }

  if (modbus_connect(Ctx) == -1)
  {
    printf("modbus_connect: %s\n", modbus_strerror(errno));
    modbus_free(Ctx);
    return -1;
  }

  modbus_set_response_timeout(Ctx, 5, 0);
  modbus_set_debug(Ctx, 1);

  while (1)
  {
    for (uint32_t Slave = 1; Slave <= MaxSlaves; Slave++)
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
  }

  modbus_close(Ctx);
  modbus_free(Ctx);

  return 0;
}

static void TransactSlave(modbus_t *Ctx)
{
  int Rc;
  const int RegCount = 4;
  int Registers[] = { 35000, 36000, 2999, 33000 } ;
  uint16_t RegValue;

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
  }
}

