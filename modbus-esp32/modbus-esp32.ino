#include <ModbusMaster.h>

// serial2 GPIO pins
#define RXD2 16
#define TXD2 17

// info we're interested in from the inverter - matches JSON names
// used by the Solis API
typedef struct {
  uint16_t batteryCapacitySoc; // (%)
  double   batteryPower; // (kW)
  double   pac;    // generation (kW)
  double   psum;   // grid in/out (kW)
  double  familyLoadPower; // load (kW)
} ModbusSolisRegister_t;

// state machine...
typedef enum { SYNC_INIT, SYNC_LOGGER, WAIT_IDLE, MODBUS_REQUEST } SolisState_t ;
static SolisState_t SolisState = SYNC_INIT ;

// the modbusmaster instance
static ModbusMaster ModbusInst;

static bool ModBusReadSolisRegisters( ModbusSolisRegister_t *ModbusSolisRegisters)
{
  uint8_t Rc ;
  bool Ret = false;

  // most of what we need is in a single grouping
  // see RS485_MODBUS-Hybrid-BACoghlan-201811228-1854.pdf
  // These are read in one transaction:
  // 33135: battery status 0=charge, 1=discharge (battery current direction)
  // 33139: Battery capacity SOC
  // 33147: House load power
  // 33149:33150: Battery power
  Rc = ModbusInst.readInputRegisters(33135,16) ;
  if ( Rc == ModbusInst.ku8MBSuccess)
  {
    ModbusSolisRegisters->batteryCapacitySoc = ModbusInst.getResponseBuffer(4) ; // 33139
    ModbusSolisRegisters->batteryPower = (ModbusInst.getResponseBuffer(14) << 16) + ModbusInst.getResponseBuffer(15);  // 33149:33150
    // 33135 - battery charge status, 0=charge, 1=discharge
    if (ModbusInst.getResponseBuffer(0))
      ModbusSolisRegisters->batteryPower *= -1;  // if discharging, flip the power
    ModbusSolisRegisters->batteryPower/=1000.0 ; // convert to kW
    ModbusSolisRegisters->familyLoadPower = (double)ModbusInst.getResponseBuffer(12) / 1000 ; // 33147

    // two further register reads are required to get the remaining info...
    // 33057:33058: Current Generation
    // 33263:33264: Meter total active power
    Rc = ModbusInst.readInputRegisters(33057,2) ;
    if ( Rc == ModbusInst.ku8MBSuccess)
    {
      uint32_t Generation = (ModbusInst.getResponseBuffer(0) << 16) + ModbusInst.getResponseBuffer(1);   // expressed in watts
      ModbusSolisRegisters->pac = (double)Generation / 1000 ;  // return as kW

      Rc = ModbusInst.readInputRegisters(33263,2) ;
      if ( Rc == ModbusInst.ku8MBSuccess)
      {
        int32_t ActivePower = (ModbusInst.getResponseBuffer(0) << 16) + ModbusInst.getResponseBuffer(1) ;
        ModbusSolisRegisters->psum = (double)ActivePower * 0.001;
        Ret = true;
      }
      else
        Serial.printf("readInputRegisters: %d\n", Rc) ;
    }
    else
      Serial.printf("readInputRegisters: %d\n", Rc) ;
  }
  else
    Serial.printf("readInputRegisters: %d\n", Rc) ;

  return Ret ;
}

void setup() 
{
  // put your setup code here, to run once:
  const uint8_t SlaveId = 1u ;

  // debug serial
  Serial.begin(115200);

  // modbus serial
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  // 1s rx timeout
  Serial2.setTimeout(1000) ;

  ModbusInst.begin(SlaveId,Serial2);

  Serial.println("Setup done");
}

void loop() 
{
  uint8_t ScratchBuf[256] ;
  static long InitTime, SyncTime ;
  const long MinSyncTime = 4000 ;
  const long BusIdleTime = 9900 ;  // ~10s fractionally less so we trip on the 10s rather than 11s boundary
  size_t BytesRead ;
  ModbusSolisRegister_t ModbusSolisRegisters;

  switch (SolisState)
  {
    case SYNC_INIT :

      Serial.println("Sync with logger...");
      InitTime = millis() ;
      SolisState = SYNC_LOGGER ;
      break ;

    case SYNC_LOGGER :

      // first, wait for the next burst of traffic from the logger, this normally occurs every minute
      // poll for data available
      if (Serial2.available() )
      {
        SolisState = WAIT_IDLE ;
        // save time at which data arrives
        SyncTime = millis() ;
        Serial.println("Wait for bus idle...");
      }
      else
        delay(1000) ;
      break ;

    case WAIT_IDLE :

      // data is on the bus, that's what we're waiting for

      // read any pending data, this will return when either the buffer is full or it times out (the latter most likely)
      BytesRead = Serial2.readBytes(ScratchBuf,sizeof(ScratchBuf)) ;
      if ( BytesRead )
      {
        // this replicates the condition in the prototype code to dump data which comes in unexpectedly sooner than expected
        // ie. dump it and start the re-sync process
        if ( (millis() - InitTime) < MinSyncTime )
        {
          Serial.println("Got data within 4s, re-syncing just to be sure...");
          SolisState = SYNC_INIT ;
        }
      }
      else if ( (millis() - SyncTime) > BusIdleTime )  // test for ~10s of inactivity ie. no data received in this period
      {
        // bus is now clear, we should now have a good 50s time spare now
        SolisState = MODBUS_REQUEST ;
        Serial.printf("Wait for idle elapsed %d ms\n", millis()-SyncTime);
      }
      break ;

    case MODBUS_REQUEST :

      Serial.println("Issuing request");
      if ( ModBusReadSolisRegisters( &ModbusSolisRegisters ) )
      {
          Serial.printf("Battery capacity SOC: %u%%\n", ModbusSolisRegisters.batteryCapacitySoc);
          Serial.printf("Battery power: %f kW\n", ModbusSolisRegisters.batteryPower);
          Serial.printf("House load power: %f kW\n", ModbusSolisRegisters.familyLoadPower);
          Serial.printf("Current Generation - DC power o/p: %f kW\n", ModbusSolisRegisters.pac);
          Serial.printf("Meter total active power: %f kW\n", ModbusSolisRegisters.psum);
      }

      SolisState = SYNC_INIT ;
      break ;

    default :

      Serial.println("Invalid state!");
      break ;
  }
}
