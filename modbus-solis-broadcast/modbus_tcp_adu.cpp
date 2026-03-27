#include "modbus_tcp_adu.h"
#include <stdio.h>
#ifndef WIN32
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#endif

// supported function code definitions
const uint8_t ModbusTcpAdu::FCodeReadDiscrete = 2;
const uint8_t ModbusTcpAdu::FCodeReadHolding = 3;
const uint8_t ModbusTcpAdu::FCodeReadInput = 4 ;
const uint8_t ModbusTcpAdu::FCodeWriteSingle = 6;
const uint8_t ModbusTcpAdu::FCodeWriteCoil = 5;
const uint8_t ModbusTcpAdu::FCodeWriteMultiple = 16; 

std::map<uint8_t,std::string> ModbusTcpAdu::FunctionDescriptions;

// attempt to construct a modbus TCP ADU from the supplied frame data
ModbusTcpAdu::ModbusTcpAdu(SOCKET Sfd, const uint8_t *Frame, uint32_t Len) : Sfd(Sfd)
{
  // initialise description lookup on first run - used for diagnostic reporting
  if (FunctionDescriptions.empty())
  {
    FunctionDescriptions[FCodeReadDiscrete] = "Read Discrete Inputs";
    FunctionDescriptions[FCodeReadHolding] = "Read Holding Registers";
    FunctionDescriptions[FCodeReadInput] = "Read Input Registers";
    FunctionDescriptions[FCodeWriteSingle] = "Write Single Register";
    FunctionDescriptions[FCodeWriteCoil] = "Write Single Coil";
    FunctionDescriptions[FCodeWriteMultiple] = "Write Multiple Registers";
  }

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
      RegisterData.push_back(Reg);
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
  if (send(Sfd, (const char*)Frame, Len, 0) == Len)
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
      if (RegisterData == Other.RegisterData)
        return true;
    }
    else
      return true;
  }
  return false;
}

// send completed transaction back to the client
bool ModbusTcpAdu::TcpSendResponse(SOCKET Sfd, uint16_t TransactionId) const
{
  uint8_t Frame[1500];
  uint16_t *APtr = (uint16_t*)Frame;
  uint16_t ByteCount, FrameLen = sizeof(uint8_t) ;  // unit identifier is always present in the ADU header
  uint8_t BitShift;
  uint8_t DiscreteByte;

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

    // discrete inputs are returned as a packed array of bits
    BitShift = 0;
    DiscreteByte = 0;
    for (auto It = RegisterData.begin(); It != RegisterData.end(); ++It)
    {
      auto Val = *It;

      if (Val)
        DiscreteByte |= (1 << BitShift);
      BitShift++;
      if (BitShift==8)
      {
        *DPtr++ = DiscreteByte;
        BitShift = 0;
        DiscreteByte = 0;
      }
    }
    if (BitShift)
      *DPtr++ = DiscreteByte;
    break;

  case FCodeReadHolding:
  case FCodeReadInput:

    *DPtr++ = ByteCount;
    for (auto It = RegisterData.begin(); It != RegisterData.end(); ++It)
    {
      auto Val = *It;

      *DPtr++ = Val >> 8;
      *DPtr++ = Val & 0xff;
    }
    break;

  case FCodeWriteCoil :
  case FCodeWriteSingle:

    *DPtr++ = RegisterAddress >> 8;
    *DPtr++ = RegisterAddress & 0xff;
    *DPtr++ = RegisterData.front() >> 8;
    *DPtr++ = RegisterData.front() & 0xff;
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

// create a modbus RTU session
modbus_t *ModbusTcpAdu::CreateModbusRtuSession(const char *Device, uint8_t Slave)
{
  modbus_t *Ctx = modbus_new_rtu(Device, 9600, 'N', 8, 1);

  if (!Ctx)
  {
    printf("modbus_new_rtu: %s\n", modbus_strerror(errno));
    return nullptr;
  }
  if (modbus_connect(Ctx) == -1)
  {
    printf("modbus_connect: %s\n", modbus_strerror(errno));
    modbus_free(Ctx);
    return nullptr;
  }
  if (modbus_set_slave(Ctx, Slave) == -1)
  {
    printf("modbus_set_slave: %s\n", modbus_strerror(errno));
    modbus_close(Ctx);
    modbus_free(Ctx);
    return nullptr;
  }
#ifdef WIN32
  // for testing
  modbus_set_response_timeout(Ctx, 15, 0);
  modbus_set_debug(Ctx, 1);
#endif

  return Ctx;
}

// perform requested RTU transaction
bool ModbusTcpAdu::PerformRTUTransaction(const char *Device)
{
  modbus_t *Ctx = ModbusTcpAdu::CreateModbusRtuSession(Device, UnitId);
  int Rc = -1 ;
  uint16_t *RegBank;
  uint8_t *DiscreteBank;

  if (!Ctx)
    return false;

  switch (FunctionCode)
  {
    case FCodeReadDiscrete :

      DiscreteBank = new uint8_t[RegisterCount];
      Rc = modbus_read_input_bits(Ctx, RegisterAddress, RegisterCount, DiscreteBank);
      if (Rc == RegisterCount)
      {
        for (uint16_t i = 0; i < RegisterCount; i++)
          RegisterData.push_back(DiscreteBank[i]);
        Processed = true;
      }
      delete DiscreteBank;
      break;

    case FCodeReadHolding :

      RegBank = new uint16_t[RegisterCount];
      Rc = modbus_read_registers(Ctx, RegisterAddress, RegisterCount, RegBank);
      if (Rc == RegisterCount)
      {
        for (uint16_t i = 0; i < RegisterCount; i++)
          RegisterData.push_back(RegBank[i]);
        Processed = true;
      }
      delete[] RegBank;
      break;

    case FCodeReadInput :

      RegBank = new uint16_t[RegisterCount];
      Rc = modbus_read_input_registers(Ctx, RegisterAddress, RegisterCount, RegBank);
      if (Rc == RegisterCount)
      {
        for (uint16_t i = 0; i < RegisterCount; i++)
          RegisterData.push_back(RegBank[i]);
        Processed = true;
      }
      delete[] RegBank;
      break;

    // note the write operations need to lock against changes to the data which is about
    // to be written
    case FCodeWriteCoil:
    {
      boost::lock_guard<boost::mutex> RegisterLock(WriteMutex);
      if (RegisterData.front())
        Rc = modbus_write_bit(Ctx, RegisterAddress, TRUE);
      else
        Rc = modbus_write_bit(Ctx, RegisterAddress, FALSE);
      if (Rc == 1)
        Processed = true;
      break;
    }

    case FCodeWriteSingle:
    {
      boost::lock_guard<boost::mutex> RegisterLock(WriteMutex);
      Rc = modbus_write_register(Ctx, RegisterAddress, RegisterData.front());
      if (Rc == 1)
        Processed = true;
      break;
    }

    case FCodeWriteMultiple:
    {
      boost::lock_guard<boost::mutex> RegisterLock(WriteMutex);
      Rc = modbus_write_registers(Ctx, RegisterAddress, RegisterCount, RegisterData.data());
      if (Rc == RegisterCount)
        Processed = true;
      break;
    }
  }

  modbus_close(Ctx);
  modbus_free(Ctx);

  // record the transaction time
  if (Processed)
    ProcessTime = boost::chrono::steady_clock::now();

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
