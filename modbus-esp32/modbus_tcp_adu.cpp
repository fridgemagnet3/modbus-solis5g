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
    return false;
}

// perform requested RTU transaction
bool ModbusTcpAdu::PerformRTUTransaction(const char *Device)
{
  return false ;
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

