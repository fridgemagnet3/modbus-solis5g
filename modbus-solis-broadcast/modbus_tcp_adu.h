#ifndef MODBUS_TCP_ADU_H

#define MODBUS_TCP_ADU_H

#ifdef WIN32
#include <WinSock2.h>
#else
typedef int SOCKET;
#endif
#include <stdint.h>
#include <vector>
#include <boost/chrono/chrono.hpp>
#include <modbus/modbus.h>
#include <boost/thread.hpp>
#include <sstream>
#include <map>

// class holding a Modbus TCP ADU

class ModbusTcpAdu
{
public :
  
  // constructor
  ModbusTcpAdu(SOCKET Sfd, const uint8_t *Frame, uint32_t Len);

  // has frame been deemed valid
  bool IsValidFrame(void) const
  {
    return ValidFrame;
  }

  // send a device busy for this frame
  bool TcpSendDeviceBusy(void) const ;

  // is this the same content as supplied ADU
  bool IsIdenticalAdu(const ModbusTcpAdu &Other, bool IncludeRegisterData = false) const;

  // has this ADU been processed?
  bool IsProcessed(void) const
  {
    return Processed ;
  }

  // return transaction id of this ADU
  uint16_t GetTransactionId(void) const
  {
    return TransactionId;
  }

  // register count in this transaction
  uint16_t GetRegisterCount(void) const
  {
    return RegisterCount;
  }

  // indicates if transaction is write request
  bool IsWriteTransaction(void) const
  {
    return (FunctionCode == FCodeWriteSingle) ||
      (FunctionCode == FCodeWriteMultiple) ||
      (FunctionCode == FCodeWriteCoil);
  }

  // send response frame back to TCP client
  bool TcpSendResponse( SOCKET Sfd, uint16_t TransactionId) const;

  // get register data
  const std::vector<uint16_t>& GetRegisterData(void) const
  {
    return RegisterData;
  }

  // update register data with new - used for updating a pending write call with new data
  void UpdateRegisterData(const std::vector<uint16_t>NewData)
  {
    // take the register write lock mutex to ensure
    // we can't update this whilst it's being sent to the RTU slave
    boost::lock_guard<boost::mutex> RegisterLock(WriteMutex);

    if (RegisterData != NewData)
    {
      RegisterData = NewData;
      // reset the processed flag regardless
      Processed = false;
    }
  }

  // perform the RTU transaction for this request
  bool PerformRTUTransaction(const char *Device);

  // indicates if this ADU is considered stale based on age
  bool IsStale(void) const
  {
    if (!Processed)
      return false;

    // write transactions don't ever go stale
    if (IsWriteTransaction())
      return false;

    // holding registers (apart from the clock) are mostly controls
    // so shouldn't really change...
    if ( Transaction == HOLDING_REGISTERS )
    {
      if (boost::chrono::steady_clock::now() > (ProcessTime + boost::chrono::minutes(20)))
        return true;
    }
    else
    {
      if (boost::chrono::steady_clock::now() > (ProcessTime + boost::chrono::minutes(5)))
        return true;
    }
    return false;
  }

  // checks to see if this ADU holds register data present in the supplied
  // and if so, marks it as invalid
  bool InvalidateAdu(const ModbusTcpAdu &Other);

  // generate string with transaction info - for diag purposes
  std::string GetTransactionString(void)
  {
    std::stringstream Buf;

    Buf << FunctionDescriptions[FunctionCode] << " (" << (uint32_t)FunctionCode << "), RegBase: " << RegisterAddress << " Count: " << RegisterCount;

    return Buf.str();
  }

  // create a modbus RTU session 
  static modbus_t *CreateModbusRtuSession(const char *Device, uint8_t Slave = 1);

private :

  // is register in the range defined by this ADU
  bool IsRegisterInRange(uint16_t Register) const
  {
    if (Register >= RegisterAddress && Register < (RegisterAddress + RegisterCount))
      return true;
    else
      return false;
  }

  // type of transaction
  typedef enum { COILS, DISCRETES, HOLDING_REGISTERS, INPUT_REGISTERS } Transaction_t;

  // client socket
  SOCKET Sfd;

  // frame validity flag
  bool ValidFrame = false;
  // ADU header
  uint16_t TransactionId;
  uint16_t Length;
  uint8_t UnitId;
  // Modbus function code
  uint8_t FunctionCode;
  // register base address for transaction
  uint16_t RegisterAddress;
  // no. of registers to be read/written
  uint16_t RegisterCount;
  // type of transaction
  Transaction_t Transaction;

  // register data - holds data to be written or just read 
  std::vector<uint16_t> RegisterData;

  // indicates if this ADU has been processed 
  bool Processed = false;

  // timestamp when this transaction was processed
  boost::chrono::steady_clock::time_point ProcessTime;

  // mutex used to lock write register transactions
  boost::mutex WriteMutex;

  // supported function codes
  static const uint8_t FCodeReadDiscrete;
  static const uint8_t FCodeReadHolding;
  static const uint8_t FCodeReadInput;
  static const uint8_t FCodeWriteCoil;
  static const uint8_t FCodeWriteSingle;
  static const uint8_t FCodeWriteMultiple;

  static std::map<uint8_t,std::string> FunctionDescriptions;

};

#endif
