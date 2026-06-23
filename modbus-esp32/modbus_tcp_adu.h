#ifndef MODBUS_TCP_ADU_H

#define MODBUS_TCP_ADU_H

#include <stdint.h>
#include <map>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <Arduino.h>
#include <ModbusMaster.h>

#define MAX_ADUS 100

// this matches the upper limit supported by ModbusMaster
#define MAX_REGISTERS 125

// class holding a Modbus TCP ADU

class ModbusTcpAdu
{
public :

  // type of transaction
  typedef enum { COILS, DISCRETES, HOLDING_REGISTERS, INPUT_REGISTERS } Transaction_t;

  // init memory pool, must be called before instantiating a class
  static void MemPoolInit(void) ;

  // constructor
  ModbusTcpAdu(int Sfd, const uint8_t *Frame, uint32_t Len);

  // memory allocators
  void *operator new(size_t Size)
  {
    void *Ptr ;
    return xQueueReceive(PoolQueue, &Ptr, 0) ? Ptr : nullptr;
  }

  void operator delete(void *Ptr)
  {
    xQueueSend(PoolQueue, &Ptr, 0);
  }

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
  bool TcpSendResponse( int Sfd, uint16_t TransactionId) const;

  // get register data
  const uint16_t *GetRegisterData(void) const
  {
    return RegisterData;
  }

  // update register data with new - used for updating a pending write call with new data
  void UpdateRegisterData(const uint16_t *NewData)
  {
    // take the register write lock mutex to ensure
    // we can't update this whilst it's being sent to the RTU slave
    xSemaphoreTake(WriteMutex,portMAX_DELAY) ;

    if ( memcmp(NewData,RegisterData,RegisterCount*sizeof(uint16_t)))
    {
      memcpy(RegisterData,NewData,RegisterCount*sizeof(uint16_t)) ;
      // reset the processed flag regardless
      Processed = false;
    }
    xSemaphoreGive(WriteMutex) ;
  }

  // update register read data with new
  bool UpdateRegisterReadData(uint16_t Address, uint16_t Data, Transaction_t Transaction)
  {
    // only applies if ADU has already been processed, not a write and the register is
    // part of this transaction
    if (Processed && !IsWriteTransaction() && Transaction == this->Transaction && IsRegisterInRange(Address))
    {
      uint16_t Offset = Address - RegisterAddress;

      RegisterData[Offset] = Data;

      return true;
    }
    return false;
  }

  // perform the RTU transaction for this request
  bool PerformRTUTransaction(ModbusMaster &ModbusInst);

  // indicates if this ADU is considered stale based on age
  bool IsStale(void) const
  {
    if (!Processed)
      return false;

    // write transactions don't ever go stale
    if (IsWriteTransaction())
      return false;

    // holding registers (apart from the clock) are mostly controls
    // so shouldn't really change that often
    if ( Transaction == HOLDING_REGISTERS )
    {
      if ( millis()-ProcessTime > 5*60*1000 )
        return true;
    }
    else
    {
      // fractionally over 1 minute to allow for HA Solis Modbus slow poll interval plus
      // our "normal" poll of 16s
      if ( millis()-ProcessTime > 80*1000 )
        return true;
    }

    return false;
  }

  // checks to see if this ADU holds register data present in the supplied
  // and if so, marks it as invalid
  bool InvalidateAdu(const ModbusTcpAdu &Other);

  // generate string with transaction info - for diag purposes
  void GetTransactionString(char *Buf, uint32_t BufSz)
  {
    snprintf(Buf,BufSz, "%s (%hu) RegBase: %hu: Count: %hu", 
        FunctionDescriptions[FunctionCode], (uint16_t)FunctionCode, RegisterAddress, RegisterCount) ;
  }

private :

  // is register in the range defined by this ADU
  bool IsRegisterInRange(uint16_t Register) const
  {
    if (Register >= RegisterAddress && Register < (RegisterAddress + RegisterCount))
      return true;
    else
      return false;
  }

  // client socket
  int Sfd;

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
  uint16_t RegisterData[MAX_REGISTERS];

  // indicates if this ADU has been processed 
  bool Processed = false;

  // timestamp when this transaction was processed
  unsigned long ProcessTime;

  // mutex used to lock write register transactions
  StaticSemaphore_t  MutexBuffer;
  SemaphoreHandle_t WriteMutex;

  // supported function codes
  static const uint8_t FCodeReadDiscrete;
  static const uint8_t FCodeReadHolding;
  static const uint8_t FCodeReadInput;
  static const uint8_t FCodeWriteCoil;
  static const uint8_t FCodeWriteSingle;
  static const uint8_t FCodeWriteMultiple;

  static std::map<uint8_t,const char*> FunctionDescriptions;

  static QueueHandle_t PoolQueue ;
};

#endif
