#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#ifdef WIN32
#include <io.h>
#pragma warning(disable : 4996)
#else
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#endif
#include <boost/crc.hpp>
#include <boost/date_time.hpp>
#include <boost/date_time/date_facet.hpp>
#include <stdlib.h>
#include <vector>
#include <iostream>
#include <sstream>

// App designed to sniff, decode and optionally capture
// the modbus data sent between a Solis inverter
// and the wifi datalogger

// modbus functions, note that according to the documentation, the solis inverters only
// support functions 3, 4, 6 and 16
static const uint32_t CmdCount = 18;
static const char *CmdLookup[] = {
  "Unknown",
  "Read Coils",
  "Read Discrete Inputs",
  "Read Holding Registers",
  "Read Input Registers",
  "Write Single Coil",
  "Write Single Register",
  "Unknown",
  "Diagnostics",
  "Unknown",
  "Unknown",
  "Get Comm Event Counter",
  "Unknown",
  "Unknown",
  "Unknown",
  "Write Multiple Coils",
  "Write Multiple Registers",
  "Report Server ID" };

static FILE *CsvLog ;
static FILE *BinLog ;

static int32_t Read(int Fd, void *Buf, size_t Count)
{
  uint8_t *Ptr = (uint8_t*)Buf ;
  size_t Rc ;
  size_t TotalCount = 0 ;
  
  while(Count)
  {
    // on a serial device, 'read' can return early yielding
    // less data than requested so keep going until we get it all
    Rc = read(Fd, Ptr, Count);
    if ( Rc < 0 )
      return Rc ;
    else if ( !Rc )
      return TotalCount ;
    if ( BinLog )
      fwrite(Ptr,1,Rc,BinLog) ;
    Count-=Rc ;
    Ptr+=Rc ;
    TotalCount+=Rc ;
  }
  return TotalCount ;
}

// try and locate start of next message by looking for a sequence matching the slave id
// and a valid function
static bool ReadMessageHeader(int Fd, uint8_t Slave, uint8_t &Function)
{
  uint8_t Buf;
  uint32_t Skipped = 0;

  while (Read(Fd, &Buf, sizeof(Buf)) == sizeof(Buf))
  {
    if (Slave == Buf)
    {
      if (Read(Fd, &Buf, sizeof(Buf)) == sizeof(Buf))
      {
        // an error response from the inverter sets the top bit in the function code
        if ( (Buf & 0x7f) < CmdCount)
        {
          Function = Buf;
          printf("Skipped %u bytes in stream looking for next header\n",Skipped);
          return true;
        }
      }
      else
        return false;
    }
    Skipped++;
  }
  printf("Failed to locate start of a request/response\n");
  return false;
}

// process a command request
static bool ProcessRequest(int Fd, uint8_t Slave, uint8_t &Function,bool &Valid,std::vector<uint16_t> &ResponseData)
{
  using namespace boost::posix_time;
  uint8_t Request[256];
  uint8_t Len, ByteCount;
  static ptime LastRequestTime ;
  
  Valid = false;
  ResponseData.clear();

  if ( BinLog )
    printf("BinLog Position: %08x\n", ftell(BinLog)) ;
    
  if (ReadMessageHeader(Fd,Slave,Function) )
  {
    // compute the expected length based on the function
    switch (Function)
    {
      case 1:
      case 2:
      case 3:
      case 4:
      case 5:
      case 6:

        Len = 6;
        break;

      case 0xF:
      case 0x10:
        Len = 5;  // enough to read up to & including the byte count
        break;

      default:

        Len = 0;
        break;
    }

    ptime TimeStamp(microsec_clock::local_time());

    std::cout << std::endl << "Request... " << to_simple_string(TimeStamp) ;
    // report intervals between requests
    if ( LastRequestTime != not_a_date_time )
    {
      auto Delta = TimeStamp - LastRequestTime ;
      std::cout << " (delta since previous: " << Delta.total_seconds() << "." << Delta.fractional_seconds() << "s)" ;
    }
    std::cout << std::endl ;
    LastRequestTime = TimeStamp ;
     
    if ( CsvLog )
    {
      fprintf(CsvLog,"%s,%u",to_simple_string(TimeStamp).c_str(),Function);
      fflush(CsvLog);
    }
    printf("Slave: %u\n", Slave);
    printf("Function: ");
    if (Function < CmdCount)
      printf("%s", CmdLookup[Function]);
    else
      printf("Unknown");
    printf(" (%u)\n", Function);

    if (Len)
    {
      // read remainder of the request
      if (Read(Fd, Request, Len) == Len)
      {
        uint16_t Crc = (Request[5] << 8) + Request[4];
        auto ModBusCrc = boost::crc_optimal<16, 0x8005, 0xFFFF, 0, true, true> {};
        uint16_t Address;

        ModBusCrc.process_byte(Slave);
        ModBusCrc.process_byte(Function);

        Address = (Request[0] << 8) + Request[1];

        printf("Address: %u\n", Address);
        if ( CsvLog )
          fprintf(CsvLog,",%u\n",Address);
          
        // make the address the first entry in the response buffer
        ResponseData.push_back(Address);

        ModBusCrc.process_bytes(Request, 4);

        switch (Function)
        {
        case 1:

          printf("Quantity of Coils: %u\n", (Request[2] << 8) + Request[3]);
          break;

        case 2:

          printf("Quantity of Inputs: %u\n", (Request[2] << 8) + Request[3]);
          break;

        case 3:
        case 4:

          printf("Quantity of Registers: %u\n", (Request[2] << 8) + Request[3]);
          break;

        case 5:
        case 6:

          printf("Write Data: %u\n", (Request[2] << 8) + Request[3]);
          break;

        case 15:
        case 16:

          if ( Function == 15 )
            printf("Quantity of coils: %u\n", (Request[2] << 8) + Request[3]);
          else
            printf("Quantity of Registers: %u\n", (Request[2] << 8) + Request[3]);

          ByteCount = Request[4];
          printf("Byte Count: %u\n", ByteCount);
          Len = ByteCount + sizeof(uint16_t);
          if (Read(Fd, Request, Len) == Len)
          {
            for (auto i = 0u; i < ByteCount; i += 2)
              printf("Write Data: %u\n", (Request[i] << 8) + Request[i + 1]);

            Crc = (Request[Len - 1] << 8) + Request[Len - 2];
            ModBusCrc.process_byte(ByteCount);
            ModBusCrc.process_bytes(Request, ByteCount);
          }
          else
          {
            perror("Failed to read data");
            return false;
          }
          break;
        }
        printf("CRC: %x - ", Crc);

        if (ModBusCrc.checksum() == Crc)
        {
          printf("Ok\n");
          Valid = true;
        }
        else
          printf("Incorrect, should be %x\n", ModBusCrc.checksum());
        return true;
      }
      else
        perror("Failed to read data");
    }
    else
      perror("Failed to read data");
  }
  else
    printf("Unrecognized command, cannot parse message\n");

  return false;
}

// process response packet
static bool ProcessResponse(int Fd,uint8_t Slave,uint8_t &Function,bool &Valid,std::vector<uint16_t> &ResponseData,bool Verbose=false)
{
  using namespace boost::posix_time;
  uint8_t Len, ByteCount;
  uint8_t Response[256];

  Valid = false;

  if (ReadMessageHeader(Fd, Slave, Function) &&
      (Read(Fd, &ByteCount, sizeof(ByteCount)) == 1))
  {
    ptime TimeStamp(microsec_clock::local_time());

    std::cout << std::endl << "Response... " << to_simple_string(TimeStamp) << std::endl;

    printf("Slave: %u\n", Slave);

    // an error response from the inverter sets the top bit in the function code
    if (Function & 0x80)
    {
      uint8_t ErrorCode = ByteCount;
      uint16_t Crc;
      auto ModBusCrc = boost::crc_optimal<16, 0x8005, 0xFFFF, 0, true, true> {};

      ModBusCrc.process_byte(Slave);
      ModBusCrc.process_byte(Function);
      ModBusCrc.process_byte(ErrorCode);

      Function &= 0x7f;
      printf("Error on function: ");
      if (Function < CmdCount)
        printf("%s", CmdLookup[Function]);
      else
        printf("Unknown");
      printf(" (%u)\n", Function);

      printf("Error code: %u\n", ErrorCode);
      if (Read(Fd, &Crc, sizeof(Crc)) == sizeof(Crc))
      {
        printf("CRC: %x - ", Crc);
        if (ModBusCrc.checksum() == Crc)
        {
          printf("Ok\n");
          Valid = true;
        }
        else
          printf("Incorrect, should be %x\n", ModBusCrc.checksum());
        return true;
      }
      else
        perror("Failed to read data");
      return false;
    }

    printf("Function: ");
    if (Function < CmdCount)
      printf("%s",CmdLookup[Function]);
    else
      printf("Unknown");
    printf(" (%u)\n", Function);
    printf("Byte Count: %u\n", ByteCount);

    // total length to read, including the CRC
    Len = ByteCount+sizeof(uint16_t);
    if (Read(Fd, Response, Len) == Len)
    {
      uint16_t Crc = (Response[Len-1]<<8) + Response[Len-2];
      uint16_t Address = 0 ;
      
      auto ModBusCrc = boost::crc_optimal<16, 0x8005, 0xFFFF, 0, true, true> {};

      ModBusCrc.process_byte(Slave);
      ModBusCrc.process_byte(Function);
      ModBusCrc.process_byte(ByteCount);

      // if available, fetch the address from the response buffer
      if ( !ResponseData.empty() )
        Address = ResponseData.front() ;
    
      for (auto i = 0u; i < ByteCount; i += 2)
      {
        uint16_t DataWord = (Response[i] << 8) + Response[i + 1];
        if ( Verbose )
          printf("Addr: %04u => %04x\n", Address+i/sizeof(uint16_t), DataWord);
        ResponseData.push_back(DataWord);
      }
      printf("CRC: %x - ", Crc);

      ModBusCrc.process_bytes(Response, ByteCount);
      if (ModBusCrc.checksum() == Crc)
      {
        Valid = true;
        printf("Ok\n");
      }
      else
        printf("Incorrect, should be %x\n", ModBusCrc.checksum());
      return true;
    }
    else
      perror("Failed to read data");
  }
  else
    perror("Failed to read data");
  return false;
}

#define YELLOW  "\033[33m"
#define WHITE   "\033[37m"

static void DecodeResponseData(uint8_t Function, std::vector<uint16_t>ResponseData)
{
  // only decoding register reads
  if (Function != 4)
    return;

  uint16_t BaseAddress = 0;

  printf( YELLOW"Decoded response (subset): \n" ) ;
  for (auto It = ResponseData.begin(); It != ResponseData.end(); ++It)
  {
    if (It == ResponseData.begin())
      BaseAddress = *It;
    else
    {
      uint16_t Address = BaseAddress + std::distance(ResponseData.begin(), It)-1;
      // The datalogger normally requests registers in batches of 25, that means (in theory)
      // you could get a 32-bit register spanning two batches - for the ones I'm interested in
      // don't think that happens but make them static anyway to be on the safe side
      static int32_t ActivePower, BattPower, GridPower, MeterTotalActivePower ;
      static uint32_t CurrentGeneration ;
      
      switch (Address)
      {
      case 33000:
        printf("%u: Product Model: %u\n", Address, *It);
        break;

      case 33022:
        printf("%u: System Time Year: %u\n", Address, *It);
        break;

      case 33023:
        printf("%u: System Time Month: %u\n", Address, *It);
        break;

      case 33024:
        printf("%u: System Time Day: %u\n", Address,*It);
        break;

      case 33025:
        printf("%u: System Time Hour: %u\n", Address, *It);
        break;

      case 33026:
        printf("%u: System Time Minute: %u\n", Address, *It);
        break;

      case 33027:
        printf("%u: System Time Second: %u\n", Address, *It);
        break;

      case 33035:
        // expressed in 0.1kWh intervals
        printf("%u:Inverter power generation today: %f kWh\n", Address, (float)(*It)*0.1) ;
        break ;

		case 33057:
		
			CurrentGeneration = ((*It) << 16) ;
			break ;
			
		case 33058:
			
			CurrentGeneration+=*It ;
			printf("%u:%u: Current Generation - DC power o/p: %u W\n", Address-1, Address, CurrentGeneration );
			break ;
			
      case 33079:
        ActivePower = ((*It) << 16) ;
        break ;
        
      case 33080:
        ActivePower+=*It ;
        printf("%u:%u: Active power: %d W\n", Address-1, Address, ActivePower) ;
        break ;
      
      case 33094:
        printf("%u: Grid frequency: %f Hz\n", Address, (*It)*0.01) ;
        break ;

      case 33130 :
        GridPower = ((*It) << 16) ;
        break ;
        
      case 33131 :
			GridPower+=*It ;
			printf("%u:%u: Grid Power, +ve export, -ve import: %d W\n",Address-1, Address, GridPower) ;
			break ;
			      
      case 33135 :
      
      	printf("%u: Batttery charge status(0=charge,1=discharge): %d\n",Address,*It);
      	break ;
      	 
      case 33139 :
        printf("%u: Battery capacity SOC: %u%%\n", Address, *It) ;
        break ;
        
      case 33140 :
        printf("%u: Battery capacity SOH: %u%%\n", Address, *It) ;
        break ;
        
      case 33147 :
        printf("%u: House load power: %u W\n", Address, *It) ;
        break ;
        
      case 33149:
        BattPower = ((*It) << 16) ;
        break ;
        
      case 33150:
        BattPower+=*It ;
        printf("%u:%u: Battery power: %d W\n", Address-1, Address, BattPower) ;
        break ;
      
      case 33163 :
        printf("%u: Battery charge today: %f kWh\n", Address, (float)(*It)*0.1) ; 
        break ;
      
      case 33171 :
        printf("%u: Grid power imported today: %u kWh\n", Address, (*It)/10) ; 
        break ;
      
      case 33263 :
      	MeterTotalActivePower = ((*It) << 16) ; 
      	break ;
      	
      case 33264 :
	      MeterTotalActivePower+=*It ;
      	printf("%u:%u: Meter total active power: %f kW\n", Address-1, Address, (double)MeterTotalActivePower*0.001);
      	break ;
      	
      }
    }
  }
  printf(WHITE) ;
}

int main(int argc, char *argv[])
{
  using namespace boost::posix_time ;
  int Fd ;
  uint8_t Slave = 1u;
  uint8_t Function;
  bool Valid;
  bool Verbose = false;
  std::vector<uint16_t> ResponseData;
  ptime Now(second_clock::local_time()) ;
  bool IsLive = false ;
  bool DecodeError = false ;
  
  // the slave is needed to allow us to try and sync up with the incoming data
	if ( argc < 2 )
	{
		printf( "Usage: modbus <input> [slave address=1] [csvlog=0] [verbose=0] [binlog=0]\n");
		return -1 ;
	}
	
#ifdef WIN32
  Fd = _open(argv[1], O_RDONLY | _O_BINARY );
#else
  Fd = open(argv[1], O_RDONLY);
#endif

	if ( Fd < 0 )
	{
		perror("Failed to open input");
		return -1 ;
	}
#ifndef WIN32

   struct stat StatBuf ;

   // determine if we're connected to the tty device (as opposed to a file)
   // this allows for error recovery if a decode error occurs
	if ( fstat(Fd,&StatBuf) == 0 )
	{
		if ( S_ISCHR(StatBuf.st_mode) )
		{
			printf("Live mode detected\n");
			IsLive = true ;
		}
	}
#endif

  if ( argc > 2)
    Slave = strtoul(argv[2], NULL, 0);

  if ( argc > 4 )
  {
    if ( strtoul(argv[4],NULL,0) )
      Verbose = true ;
  }
  
  time_facet *Facet(new time_facet("%Y-%m-%d_%H-%M-%S")) ;
  std::stringstream LogName ;

  LogName.imbue(std::locale(LogName.getloc(), Facet));

  // create the CSV logfile if requested
  if ( argc > 3 && strtoul(argv[3],NULL,0) )
  {
    LogName << Now << ".csv" ;
  
    CsvLog = fopen(LogName.str().c_str(),"wt");
    if ( !CsvLog )
    {
      std::cout << "Failed to create csv file: " << LogName.str() << std::endl ;
      return -1 ;
    }
    else
    {
      std::cout << "Writing CSV to: " << LogName.str() << std::endl ;
      fputs("TimeStamp,Function,Address\n",CsvLog);
    } 
  }
  
  // create the binlog if requested
  if ( argc > 5 && strtoul(argv[5],NULL,0) )
  {
  	 LogName.str("");
    LogName << Now << ".bin" ;
  
    BinLog = fopen(LogName.str().c_str(),"wb");
    if ( !BinLog )
    {
      std::cout << "Failed to create bin file: " << LogName.str() << std::endl ;
      return -1 ;
    }
    else
    {
      std::cout << "Writing binary to: " << LogName.str() << std::endl ;
    } 
  }
  
  // start processing traffic
  while (!DecodeError)
  {
  	 DecodeError = !ProcessRequest(Fd, Slave, Function, Valid, ResponseData) ;
  	 if ( !DecodeError )
  	 {
    	DecodeError = !ProcessResponse(Fd, Slave, Function,Valid,ResponseData,Verbose) ;
    	if ( !DecodeError && Valid )
	      DecodeResponseData(Function, ResponseData);
	 }
#ifndef WIN32
	 // if we get a decode error, try and resync the stream. In effect this
	 // just waits for at least a 30s gap in the serial stream before continuing
	 if ( DecodeError && IsLive )
	 {
	 	fd_set FdSet ;
		struct timeval TimeOut ;
		bool NextPacket = false ;
		int Rc ;
		uint8_t ScratchBuf[256] ;
					
		FD_ZERO(&FdSet);
		FD_SET(Fd,&FdSet) ;

      printf( "Decode error, attempting to re-sync\n") ;
		while(!NextPacket)
		{
			TimeOut.tv_sec = 30 ;
			TimeOut.tv_usec = 0 ;
			// poll for data
			Rc = select(Fd+1,&FdSet,NULL,NULL,&TimeOut);
			if ( Rc == 0 )
			{
				// on timeout, return to main loop
				NextPacket = true ;
				DecodeError = false ;
			}
			else if ( Rc < 0)
				break ;
			else // data still pending, read and discard it
			{
				Rc = read(Fd,ScratchBuf,sizeof(ScratchBuf) ) ;
				if ( Rc > 0 && BinLog )
					fwrite(ScratchBuf,1,Rc,BinLog) ;
			}
		}		
	 }
#endif
  }
  
  close(Fd);
  if (CsvLog)
    fclose(CsvLog);
  if(BinLog)
    fclose(BinLog);
  return 0 ;
}
