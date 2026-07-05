#include "modbus_tcp_adu.h"
#include <stdio.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/netdb.h>

// supported function code definitions
const uint8_t ModbusTcpAdu::FCodeReadDiscrete = 2;
const uint8_t ModbusTcpAdu::FCodeReadHolding = 3;
const uint8_t ModbusTcpAdu::FCodeReadInput = 4 ;
const uint8_t ModbusTcpAdu::FCodeWriteSingle = 6;
const uint8_t ModbusTcpAdu::FCodeWriteCoil = 5;
const uint8_t ModbusTcpAdu::FCodeWriteMultiple = 16; 

std::map<uint8_t,const char*> ModbusTcpAdu::FunctionDescriptions;

// memory pool
static uint8_t MemPool[MAX_ADUS][sizeof(ModbusTcpAdu)] ;
QueueHandle_t ModbusTcpAdu::PoolQueue ;

// init the memory pool 
void ModbusTcpAdu::MemPoolInit(void)
{
  PoolQueue = xQueueCreate(MAX_ADUS, sizeof(void *));
  for (int i = 0; i < MAX_ADUS; i++) 
  {
    void *Ptr = &MemPool[i];
    xQueueSend(PoolQueue, &Ptr, 0);
  }

  // initialise description lookup - used for diagnostic reporting
  FunctionDescriptions[FCodeReadDiscrete] = "Read Discrete Inputs";
  FunctionDescriptions[FCodeReadHolding] = "Read Holding Registers";
  FunctionDescriptions[FCodeReadInput] = "Read Input Registers";
  FunctionDescriptions[FCodeWriteSingle] = "Write Single Register";
  FunctionDescriptions[FCodeWriteCoil] = "Write Single Coil";
  FunctionDescriptions[FCodeWriteMultiple] = "Write Multiple Registers";
}

// attempt to construct a modbus TCP ADU from the supplied frame data
ModbusTcpAdu::ModbusTcpAdu(int Sfd, const uint8_t *Frame, uint32_t Len) : Sfd(Sfd)
{
  WriteMutex = xSemaphoreCreateMutexStatic( &MutexBuffer );

  // minimum frame length for the protocol header
  const uint32_t MinFrameLen = 7;

  if (Len < MinFrameLen)
    return;

  const uint16_t *APtr = (const uint16_t*)Frame;

  // transaction id
  TransactionId = htons(*APtr++);
  // protocol id, always 0, just skip
  APtr++;
  // bytes remaining in frame
  Length = htons(*APtr++);
  // unit id
  const uint8_t *DPtr = (const uint8_t*)APtr;

  UnitId = *DPtr++;
  // the remaining length includes the unit id
  Length--;
  // see what's left
  Len -= MinFrameLen;

  // need to have at least this much left in order to fully process the frame
  if (Length < Len)
    return;

  // start decoding the DU
  FunctionCode = *DPtr++;

  // check it's a function code we can support
  if (FunctionCode != FCodeReadDiscrete &&
    FunctionCode != FCodeReadHolding &&
    FunctionCode != FCodeWriteCoil &&
    FunctionCode != FCodeReadInput &&
    FunctionCode != FCodeWriteSingle &&
    FunctionCode != FCodeWriteMultiple)
  {
    return;
  }

  // set the transaction type
  switch (FunctionCode)
  {
  case FCodeReadDiscrete :

    Transaction = DISCRETES;
    break;

  case FCodeWriteCoil :

    Transaction = COILS;
    break;

  case FCodeReadInput :

    Transaction = INPUT_REGISTERS;
    break;

  case FCodeWriteSingle :
  case FCodeWriteMultiple :
  case FCodeReadHolding :

    Transaction = HOLDING_REGISTERS;
    break;
  }

  // all of these start with a register address
  RegisterAddress = (*DPtr) << 8;
  DPtr++;
  RegisterAddress += (*DPtr);
  DPtr++;

  // single register write is - duh, just one register
  if (FunctionCode == FCodeWriteSingle ||
    FunctionCode == FCodeWriteCoil )
    RegisterCount = 1u;
  else
  {
    // everything else supplies the count
    RegisterCount = (*DPtr) << 8;
    DPtr++;
    RegisterCount += (*DPtr);
    DPtr++;
  }

  // we can only support a fixed size amount of registers
  if ( RegisterCount > MAX_REGISTERS )
    return ;

  // save away the register write data
  if (FunctionCode == FCodeWriteSingle ||
      FunctionCode == FCodeWriteCoil ||
    FunctionCode == FCodeWriteMultiple)
  {
    // validate the byte count - should match register count * 2
    if (FunctionCode == FCodeWriteMultiple)
    {
      uint8_t ByteCount = *DPtr++;
      if (ByteCount != RegisterCount*sizeof(uint16_t))
        return;
    }

    // save the register data
    for (uint32_t i = 0; i < RegisterCount; i++)
    {
      uint16_t Reg = (*DPtr) << 8;
      DPtr++;
      Reg += (*DPtr);
      DPtr++;
      RegisterData[i] = Reg ;
    }
  }

  ValidFrame = true;  
}

bool ModbusTcpAdu::TcpSendDeviceBusy(void) const
{
  uint8_t Frame[20];
  uint16_t *APtr = (uint16_t*)Frame;
  const uint16_t FrameLen = 3;   // UnitId + Function + Exception Code
  const uint8_t ServerDeviceBusy = 6;

  // construct the TCP header
  *APtr++ = htons(TransactionId);
  *APtr++ = 0;
  *APtr++ = htons(FrameLen);

  uint8_t *DPtr = (uint8_t*)APtr;
  // modbus frame
  *DPtr++ = UnitId;
  *DPtr++ = FunctionCode | 0x80;
  *DPtr++ = ServerDeviceBusy;

  uint32_t Len = DPtr - Frame;

  // send it
  if (send(Sfd, (void*)Frame, Len, 0) == Len)
    return true;
  else
    return false;
}

bool ModbusTcpAdu::IsIdenticalAdu(const ModbusTcpAdu &Other, bool IncludeRegisterData) const
{
  // all this lot must be the same
  if (Length == Other.Length &&
    UnitId == Other.UnitId &&
    FunctionCode == Other.FunctionCode &&
    RegisterAddress == Other.RegisterAddress &&
    RegisterCount == Other.RegisterCount)
  {
    // checking the register data only pertinent for write transactions
    if (IncludeRegisterData && IsWriteTransaction())
    {
      if (!memcmp(RegisterData,Other.RegisterData,RegisterCount*sizeof(uint16_t)))
        return true;
    }
    else
      return true;
  }
  return false;
}

// send completed transaction back to the client
bool ModbusTcpAdu::TcpSendResponse(int Sfd, uint16_t TransactionId) const
{
  uint8_t Frame[1500];
  uint16_t *APtr = (uint16_t*)Frame;
  uint16_t ByteCount, FrameLen = sizeof(uint8_t) ;  // unit identifier is always present in the ADU header
  uint16_t InputCount ;
  uint8_t *IPtr ;

  // construct the TCP header
  *APtr++ = htons(TransactionId);
  *APtr++ = 0;

  switch (FunctionCode)
  {
  case FCodeReadDiscrete :

    // discretes are bit packed
    ByteCount = RegisterCount / 8;
    if ((RegisterCount % 8) != 0)
      ByteCount++;
    // Framelen in the ADU is function + bytecount + bytecount's worth of data
    FrameLen += sizeof(uint8_t) * 2 + ByteCount;
    break;

  case FCodeReadHolding :
  case FCodeReadInput :

    // ByteCount in the PDU is the register count * 2
    ByteCount = RegisterCount*sizeof(uint16_t);
    // Framelen in the ADU is function + bytecount + bytecount's worth of data
    FrameLen += sizeof(uint8_t) * 2 + ByteCount;
    break;

  case FCodeWriteSingle :
  case FCodeWriteMultiple:
  case FCodeWriteCoil :

    // FrameLen is the function code + register address and value
    // or function code + register address and count
    FrameLen += sizeof(uint8_t) + sizeof(uint16_t) * 2 ;
    break;
  }

  *APtr++ = htons(FrameLen);

  uint8_t *DPtr = (uint8_t*)APtr;
  // modbus frame
  *DPtr++ = UnitId;
  *DPtr++ = FunctionCode ;

  // now construct the remainder of the PDU based on the function code
  switch (FunctionCode)
  {
  case FCodeReadDiscrete :

    *DPtr++ = ByteCount;
    IPtr = (uint8_t*)RegisterData ;

    // discrete inputs are already bitpacked from ModbusMaster
    for (auto i = 0u ; i < ByteCount ; i++ )
      *DPtr++ = *IPtr++;
    break;

  case FCodeReadHolding:
  case FCodeReadInput:

    *DPtr++ = ByteCount;
    for (auto i = 0u ; i < RegisterCount ; i++ )
    {
      auto Val = RegisterData[i];

      *DPtr++ = Val >> 8;
      *DPtr++ = Val & 0xff;
    }
    break;

  case FCodeWriteCoil :
  case FCodeWriteSingle:

    *DPtr++ = RegisterAddress >> 8;
    *DPtr++ = RegisterAddress & 0xff;
    *DPtr++ = RegisterData[0] >> 8;
    *DPtr++ = RegisterData[0] & 0xff;
    break;

  case FCodeWriteMultiple:

    *DPtr++ = RegisterAddress >> 8;
    *DPtr++ = RegisterAddress & 0xff;
    *DPtr++ = RegisterCount >> 8;
    *DPtr++ = RegisterCount & 0xff;
    break;
  }

  uint32_t Len = DPtr - Frame;

  // send it
  if (send(Sfd, (const char*)Frame, Len, 0) == Len)
    return true;
  else
    return false;
}

// perform requested RTU transaction
bool ModbusTcpAdu::PerformRTUTransaction(ModbusMaster &ModbusInst)
{
  int Rc = -1 ;

  switch (FunctionCode)
  {
    case FCodeReadDiscrete :

      Rc = ModbusInst.readDiscreteInputs(RegisterAddress,RegisterCount) ;
      if ( Rc == ModbusInst.ku8MBSuccess)
      {
        // unlike libmodbus, the discretes are directly returned as a packed
        // array of bits, padded to the next 16-bit boundary
        uint16_t InputCount = RegisterCount / 16 ;

        if ( RegisterCount % 16 )
          InputCount++ ;

        for (uint16_t i = 0; i < InputCount; i++)
          RegisterData[i] = ModbusInst.getResponseBuffer(i) ;
        Processed = true;
      }
      break;

    case FCodeReadHolding :

      Rc = ModbusInst.readHoldingRegisters(RegisterAddress,RegisterCount) ;
      if ( Rc == ModbusInst.ku8MBSuccess)
      {
        for (uint16_t i = 0; i < RegisterCount; i++)
          RegisterData[i] = ModbusInst.getResponseBuffer(i) ;
        Processed = true;
      }
      break; 

    case FCodeReadInput :

      Rc = ModbusInst.readInputRegisters(RegisterAddress,RegisterCount) ;
      if ( Rc == ModbusInst.ku8MBSuccess)
      {
        for (uint16_t i = 0; i < RegisterCount; i++)
          RegisterData[i] = ModbusInst.getResponseBuffer(i) ;
        Processed = true;
      }
      break;

    // note the write operations need to lock against changes to the data which is about
    // to be written
    case FCodeWriteCoil:
    
      xSemaphoreTake(WriteMutex,portMAX_DELAY) ;
      if (RegisterData[0])
        Rc = ModbusInst.writeSingleCoil(RegisterAddress,0xFF);
      else
        Rc = ModbusInst.writeSingleCoil(RegisterAddress,0x00);

      if ( Rc == ModbusInst.ku8MBSuccess)
        Processed = true;
      xSemaphoreGive(WriteMutex) ;
      break;
    
    case FCodeWriteSingle:

      xSemaphoreTake(WriteMutex,portMAX_DELAY) ;
      Rc = ModbusInst.writeSingleRegister(RegisterAddress,RegisterData[0]);
      if (Rc == ModbusInst.ku8MBSuccess)
        Processed = true;
      xSemaphoreGive(WriteMutex) ;
      break;

    case FCodeWriteMultiple:

      xSemaphoreTake(WriteMutex,portMAX_DELAY) ;
      for(auto i=0u ; i< RegisterCount ; i++)
        ModbusInst.setTransmitBuffer(i,RegisterData[i]);

      Rc = ModbusInst.writeMultipleRegisters(RegisterAddress,RegisterCount);
      if (Rc == ModbusInst.ku8MBSuccess)
        Processed = true;
      xSemaphoreGive(WriteMutex) ;
      break;
  }

  // record the transaction time
  if (Processed)
    ProcessTime = millis();

  return Processed;
}

// cross check register range held in supplied ADU against ours
// and if there is an overlap, mark as invalid
bool ModbusTcpAdu::InvalidateAdu(const ModbusTcpAdu &Other)
{
  if (Transaction == Other.Transaction)
  {
    if (IsRegisterInRange(Other.RegisterAddress))
    {
      Processed = false;
      return true;
    }
    if (Other.IsRegisterInRange(RegisterAddress))
    {
      Processed = false;
      return true;
    }
  }
  return false;
}

