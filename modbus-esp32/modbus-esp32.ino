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
#include <CRC16.h>

// Module: ESP32-WROOM-DA Module

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
static bool ModBusReadSolisRegisters( ModbusSolisRegister_t *ModbusSolisRegisters,unsigned long &Elapsed)
{
  uint8_t Rc ;
  bool Ret = false;
  const uint32_t TransactDelay = 80u ; // delay between each register request
  unsigned long StartTransact = millis() ;

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

  Elapsed = millis() - StartTransact ;

  return Ret ;
}

// generate JSON message aligned to Solis API from the register data
static char *GenerateJson(const ModbusSolisRegister_t *ModbusSolisRegisters, uint32_t LoggerFail)
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

// decode Modbus request and if not intended for our slave, respond with something
// that will (hopefully) persuade the logger to stop querying it
static int DecodeAndRespondToSlave(uint8_t *Buffer, uint32_t BufSz, uint8_t SlaveId)
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
  
  // messages we're expecting should all be single register read requests, 8 bytes long
  if (BufSz < MinMsgLen)
  {
    Serial.println("Message too short for a read input register function");
    return -1;
  }
  // check slave not us
  ReqSlave = Buffer[MsgStart];
  if (ReqSlave == SlaveId)
  {
    Serial.println("Message is for local inverter, ignoring");
    return SlaveId;
  }

  // check this is a read register request
  if (Buffer[MsgStart+1] != FCodeReadInput)
  {
    Serial.println("Not a read input registers function, ignoring");
    return -1;
  }

  // compute then verify the CRC
  CRC16 ModBusCrc(CRC16_ARC_POLYNOME,CRC16_CCITT_FALSE_INITIAL,CRC16_XOR_OUT,CRC16_ARC_REV_IN,CRC16_ARC_REV_OUT);

  ModBusCrc.add(&Buffer[MsgStart], MinMsgLen - sizeof(uint16_t));

  Crc = (Buffer[MsgStart+7] << 8) + Buffer[MsgStart+6];
  if (ModBusCrc.calc() != Crc)
  {
    Serial.printf("CRC Incorrect, should be %x\n", ModBusCrc.calc());
    return -1;
  }

  // extract the register
  Register = (Buffer[MsgStart+2] << 8) + Buffer[MsgStart+3];

  Serial.printf("Message for slave: %u, register: %u\n", ReqSlave, Register);

  // got what we need, now generate the response...
  uint8_t ResponseBuf[5];
  const uint8_t ExceptionIllegalData = 0x02;

  // initial attempt: respond with an illegal address exception
  ResponseBuf[0] = ReqSlave;
  ResponseBuf[1] = FCodeReadInput | 0x80;
  ResponseBuf[2] = ExceptionIllegalData;

  ModBusCrc.restart();
  ModBusCrc.add(ResponseBuf, 3);
  ResponseBuf[3] = ModBusCrc.calc() & 0xff;
  ResponseBuf[4] = ModBusCrc.calc() >> 8 ;

  delay(150) ;
  ModbusPreTransmit() ;
  Serial2.write(ResponseBuf, sizeof(ResponseBuf)) ;
  Serial2.flush(true) ;
  ModbusPostTransmit() ;

  return ReqSlave;
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
  // set rx timeout to ~200ms
  // This *should* ensure every read call gives us a single Modbus request
  // and possibly the response although we don't really care about those
  Serial2.setTimeout(200) ;

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
  static unsigned long BusIdleTime = 8000u ; 
  const unsigned long LoggerTimeout = 300*1000u ;  // 5min
  const uint32_t PollDelay = 16000;  // 16s
  const uint32_t PollThreshold = 5000u;  // 5 seconds
  size_t BytesRead ;
  ModbusSolisRegister_t ModbusSolisRegisters;
  char *jSon;
  static uint32_t LoggerFail = 0u ;
  static bool Slave10Tx = false ;
  unsigned long Elapsed ;
  static unsigned long TimeToNextPoll ;

  switch (SolisState)
  {
    case SYNC_INIT :

      Serial.println("Sync with logger...");
      SolisState = SYNC_LOGGER ;
      break ;

    case SYNC_LOGGER :

      // first, wait for the next burst of traffic from the logger, this normally occurs every minute
      // poll for data available
      if (Serial2.available() )
      {
        SolisState = CONSUME_LOGGER ;
        InitTime = millis() ;
        Serial.println("Consume logger data...");
      }
      else
      {
        delay(500) ;
        if ( (millis() - InitTime) > LoggerTimeout )
        {
          Serial.println("Timed out waiting for logger activity - going ahead anyway...");
          LoggerFail++ ;
          // do one cycle then come back here
          TimeToNextPoll = 1000u ;
          SolisState = MODBUS_REQUEST ;
        }
      }
      break ;
      
    case CONSUME_LOGGER :

      // data is on the bus, that's what we're waiting for

      // read any pending data, this will return when either the buffer is full or it times out (the latter most likely)
      BytesRead = Serial2.readBytes(ScratchBuf,sizeof(ScratchBuf)) ;
      if ( BytesRead )
      {
        int ReqSlave = DecodeAndRespondToSlave(ScratchBuf, BytesRead, 1u);
        if ( ReqSlave == 10 )
          Slave10Tx = true ;
        else if ( ReqSlave == 2 ) // if there are multiple polls this cycle, make sure we reset the Tx flag
          Slave10Tx = false ;

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
        if ( !Slave10Tx )
          BusIdleTime = 30*1000 ;  // 30s
        else
          BusIdleTime = 8*1000 ;   // 8s
      }
      else
      {
        // had no data for 200ms
        SolisState = WAIT_IDLE ;
        // save time at which data stops arriving
        SyncTime = millis() ;
      }
      break ;

    case WAIT_IDLE :

      // wait for 8s period of inactivity
      if (Serial2.available() )
        SolisState = CONSUME_LOGGER ;
      else if ( (millis() - SyncTime) > BusIdleTime )  // test for ~8s of inactivity ie. no data received in this period
      {
        // bus is now clear
        SolisState = MODBUS_REQUEST ;
        Elapsed = millis()-InitTime ;

        Serial.printf("Elapsed: %u seconds\n", Elapsed/1000u);

        // work out how much time we have till the next logger poll is due
        if ( Elapsed < LoggerTimeout )
        {
          const unsigned long MinElapsed = 60*1000 ;

          // handle the (most likely startup) condition where we happen to 
          // immediately detect traffic & therefore get a much shorter than expected elapsed time
          if ( Elapsed < MinElapsed )
            TimeToNextPoll = LoggerTimeout - MinElapsed ;
          else
            TimeToNextPoll = LoggerTimeout - Elapsed;
        }
        else
          TimeToNextPoll = 1000u;  // if was longer than expected just do the one transaction, then resync
      }
      else
        delay(200);
      break ;

    case MODBUS_REQUEST :

      // Prior to making a request, check to see if any serial data has come in
      // Normally there shouldn't be but when the logger performs it's daily reset
      // (which can happen at any time) this can easily happen, in which case force a resync
      if (Serial2.available() )
      {
        Serial.println("Detected serial traffic, forcing resync") ;
        SolisState = SYNC_INIT ;
        break ;
      }
      Serial.printf("Time to next poll: %u seconds\n", TimeToNextPoll/1000u);

      if ( TimeToNextPoll )
      {
        Serial.println("Issuing request");
        if ( ModBusReadSolisRegisters( &ModbusSolisRegisters, Elapsed ) )
        {
            Serial.printf("Battery capacity SOC: %u%%\n", ModbusSolisRegisters.batteryCapacitySoc);
            Serial.printf("Battery power: %f kW\n", ModbusSolisRegisters.batteryPower);
            Serial.printf("House load power: %f kW\n", ModbusSolisRegisters.familyLoadPower);
            Serial.printf("Current Generation - DC power o/p: %f kW\n", ModbusSolisRegisters.pac);
            Serial.printf("Meter total active power: %f kW\n", ModbusSolisRegisters.psum);

            // generate the JSON data, aligned to the Solis API
            jSon = GenerateJson(&ModbusSolisRegisters,LoggerFail);
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
          Serial.printf("Failed to retrieve modbus data from inverter\n");

        // update how much time we have left till the next poll
        if (TimeToNextPoll > (PollDelay+Elapsed))
        {
          TimeToNextPoll -= (PollDelay+Elapsed);
          // don't go down to the wire
          if (TimeToNextPoll < PollThreshold)
            TimeToNextPoll = 0u;
        }
        else
          TimeToNextPoll = 0u; 
      }

      // dont't delay on final cycle
      if ( TimeToNextPoll) 
        delay(PollDelay);
      else
        SolisState = SYNC_INIT ; // back to sync with logger
      break ;

    default :

      Serial.println("Invalid state!");
      break ;
  }
}
