#include <ModbusMaster.h>
#include <cJSON.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include "config.h"
#include <time.h>
#include <lwip/err.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/netdb.h>

// info we're interested in from the inverter - matches JSON names
// used by the Solis API
typedef struct {
  uint16_t batteryCapacitySoc; // (%)
  double   batteryPower; // (kW)
  double   pac;    // generation (kW)
  double   psum;   // grid in/out (kW)
  double  familyLoadPower; // load (kW)
  double   etoday;  // generation today (kW)
} ModbusSolisRegister_t;

// state machine...
typedef enum { SYNC_INIT, SYNC_LOGGER, CONSUME_LOGGER, WAIT_IDLE, MODBUS_REQUEST } SolisState_t ;
static SolisState_t SolisState = SYNC_INIT ;

// the modbusmaster instance
static ModbusMaster ModbusInst;

// UDP broadcast stuff
static int sFd = -1 ;
static struct sockaddr_in BroadcastAddr ; 

// read the required registers from modbus
static bool ModBusReadSolisRegisters( ModbusSolisRegister_t *ModbusSolisRegisters)
{
  uint8_t Rc ;
  bool Ret = false;
  const uint32_t TransactDelay = 80u ; // delay between each register request

  memset(ModbusSolisRegisters,0,sizeof(ModbusSolisRegister_t)) ;

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

    // delay between executing individual requests. Looking at the timing of the traffic via 'modbus-sniffer' shows that
    // the wifi logger waits anywhere betwen 40-100ms between requests. The modbus spec itself also mandates that a minimum
    // 3.5 character delay should be present between requests. This also helps (I think) in terms of discarding any frameing errors
    // injected by the transceivers turning on and off
    delay(TransactDelay);
    
    // three further register reads are required to get the remaining info...
    // 33057:33058: Current Generation
    // 33263:33264: Meter total active power
    // 33035 Current generation today
    Rc = ModbusInst.readInputRegisters(33057,2) ;
    if ( Rc == ModbusInst.ku8MBSuccess)
    {
      uint32_t Generation = (ModbusInst.getResponseBuffer(0) << 16) + ModbusInst.getResponseBuffer(1);   // expressed in watts
      ModbusSolisRegisters->pac = (double)Generation / 1000 ;  // return as kW

      delay(TransactDelay);
      Rc = ModbusInst.readInputRegisters(33263,2) ;
      if ( Rc == ModbusInst.ku8MBSuccess)
      {
        int32_t ActivePower = (ModbusInst.getResponseBuffer(0) << 16) + ModbusInst.getResponseBuffer(1) ;
        ModbusSolisRegisters->psum = (double)ActivePower * 0.001;

        delay(TransactDelay);
        Rc = ModbusInst.readInputRegisters(33035,1) ;
        if ( Rc == ModbusInst.ku8MBSuccess)
        {
          // expressed in 0.1kWh intervals
          ModbusSolisRegisters->etoday = (float)(ModbusInst.getResponseBuffer(0))*0.1;
          Ret = true;
        }
        else
          Serial.printf("readInputRegisters 33035: %x\n", Rc) ;
      }
      else
        Serial.printf("readInputRegisters 33263: %x\n", Rc) ;
    }
    else
      Serial.printf("readInputRegisters 33057: %x\n", Rc) ;
  }
  else
    Serial.printf("readInputRegisters 33135-33150: %x\n", Rc) ;

  return Ret ;
}

// generate JSON message aligned to Solis API from the register data
static char *GenerateJson(const ModbusSolisRegister_t *ModbusSolisRegisters)
{
  cJSON *SolarJson = cJSON_CreateObject();
  cJSON *Node;
  cJSON *SolarData;
  char *Ret;
  time_t TimeStamp ;
  char TimeBuf[80] ;

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
    TimeStamp = time(NULL) ;
    snprintf(TimeBuf,sizeof(TimeBuf), "%llu", TimeStamp*1000u) ;
    Node = cJSON_CreateString(TimeBuf);
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

static void GetNtpTime(void)
{
  const long GmOffsetSec = 0;
  const int  DaylightOffsetSec = 3600;

  // once initially set, this should periodically re-sync to the server
  configTime(GmOffsetSec, DaylightOffsetSec, NTP_SERVER);
}

// modbus callbacks invoked before and after transmission
static void ModbusPreTransmit(void)
{
  // enable the transmitter
  digitalWrite(RS485_DIR, HIGH);
}

static void ModbusPostTransmit(void)
{
  // disable the transmitter (in the process, re-enable the receiver)
  digitalWrite(RS485_DIR, LOW);
}

void setup() 
{
  IPAddress LocalIp ;
  int EnBroadcast = 1 ;

  // debug serial
  Serial.begin(115200);

  // setup the RS485 transceiver enable pin and default to receive
  pinMode(RS485_DIR, OUTPUT);
  digitalWrite(RS485_DIR, LOW);

  // modbus serial
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  // 1s rx timeout
  Serial2.setTimeout(1000) ;

  ModbusInst.begin(MODBUS_SLAVE_ID,Serial2);
  // setup the callbacks for enabling/disabling the transceivers
  ModbusInst.preTransmission(ModbusPreTransmit);
  ModbusInst.postTransmission(ModbusPostTransmit);

  // connect to wifi
  Serial.println("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID1, PWD1);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(F("."));
  }
  LocalIp = WiFi.localIP();
  Serial.println(F("WiFi connected"));
  Serial.println(LocalIp);

  // fetch current time
  GetNtpTime() ;

  // create the UDP socket & configure for broadcasts
  sFd = socket(PF_INET,SOCK_DGRAM,0) ;
  if ( sFd < 0 )
    Serial.println("Failed to create UDP socket");
  else
  {
    if ( setsockopt(sFd, SOL_SOCKET, SO_BROADCAST, (char*)&EnBroadcast, sizeof(EnBroadcast)) < 0 )
      Serial.println("Failed set broadcast option on UDP socket");

    memset((void*)&BroadcastAddr, 0, sizeof(struct sockaddr_in));
    BroadcastAddr.sin_family = PF_INET;
    BroadcastAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    BroadcastAddr.sin_port = htons(52005);
  }
  Serial.println("Setup done");
}

void loop() 
{
  uint8_t ScratchBuf[256] ;
  static unsigned long InitTime, SyncTime ;
  const unsigned long MinSyncTime = 4000u ;
  const unsigned long BusIdleTime = 8000u ; 
  // should have 50s worth of free time, which allows for 3 requests at 20s intervals
  const uint32_t RequestsPerCycle = 3;
  const uint32_t PollDelay = 20000;
  size_t BytesRead ;
  ModbusSolisRegister_t ModbusSolisRegisters;
  char *jSon;
  static uint32_t RequestCycle = 0 ;

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
        SolisState = CONSUME_LOGGER ;
        Serial.println("Consume logger data...");
      }
      else
        delay(500) ;
      break ;
      
    case CONSUME_LOGGER :

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
      else
      {
        // had no data for 1s
        SolisState = WAIT_IDLE ;
        // save time at which data stops arriving
        SyncTime = millis() ;
        Serial.printf("Wait for data elapsed %d ms\n", millis()-InitTime);
        Serial.println("Wait for bus idle...");
      }
      break ;

    case WAIT_IDLE :

      // wait for 10s period of inactivity
      if (Serial2.available() )
        SolisState = CONSUME_LOGGER ;
      else if ( (millis() - SyncTime) > BusIdleTime )  // test for ~10s of inactivity ie. no data received in this period
      {
        // bus is now clear, we should now have a good 50s time spare now
        SolisState = MODBUS_REQUEST ;
        Serial.printf("Wait for idle elapsed %d ms\n", millis()-SyncTime);
      }
      else
        delay(200);
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

          // generate the JSON data, aligned to the Solis API
          jSon = GenerateJson(&ModbusSolisRegisters);
          if (jSon)
          {
            Serial.printf("JSON data: %s:\n", jSon);

            if ( sendto(sFd, jSon, strlen(jSon), 0, (struct sockaddr*) &BroadcastAddr, sizeof(struct sockaddr_in)) < 0 )
              Serial.println("Failed to send broadcast packet") ;
            free(jSon);
          }
          else
            Serial.printf("Failed to encode JSON data\n");
      }
      else
      {
        // if a transaction fails, don't attempt any more in this window because the timings are likely to
        // be screwed up, restart the state machine to sync with the wifi logger
        RequestCycle = 0u ;
        SolisState = SYNC_INIT ;
        Serial.printf("Failed to retrieve modbus data from inverter\n");
        break ;
      }

      // should have 50s worth of free time, which allows for 3 requests at 20s intervals
      RequestCycle++ ;
      if ( RequestCycle == RequestsPerCycle )
      {
        RequestCycle = 0u ;
        SolisState = SYNC_INIT ;
      }
      else
        delay(PollDelay);
      break ;

    default :

      Serial.println("Invalid state!");
      break ;
  }
}
