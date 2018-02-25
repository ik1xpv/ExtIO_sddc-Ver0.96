#include "license.txt" // Oscar Steila ik1xpv

#define EXTIO_EXPORTS
#define NUM_CARRIER         2
#define LO_MIN				100000LL
#define LO_MAX				7500000000LL
#define LO_PRECISION		5000L


#define BORLAND				0

#define  _WIN32_WINNT 0x0501
#include <windows.h>
#include <wingdi.h>
#include <pthread.h>

#include <string.h>
#include <stdio.h>
#include <math.h>
#include "mytypes.h"
#include "config.h"
#include "ExtIO_sddc.h"
#include "openFX3.h"
#include "BBRF103.h"
#include "resource.h"
#include "uti.h"
#include "tdialog.h"
#include "r2iq.h"
#include "fftw3.h"
#include "splashwindow.h"
#include "r820t2.h"
#include "Si5351.h"
#define   snprintf	_snprintf

static bool SDR_supports_settings = false;  // assume not supported
static bool SDR_settings_valid = false;		// assume settings are for some other ExtIO

static char SDR_progname[32+1] = "\0";
static int  SDR_ver_major = -1;
static int  SDR_ver_minor = -1;

static int		gHwType = exthwUSBfloat32;

static int		giExtSrateIdx = 0;
static unsigned gExtSampleRate = 32000000;


volatile double	gaCarrierAmp[NUM_CARRIER];
volatile double	gaCarrierPhaseInc[NUM_CARRIER];

volatile int64_t	glLOfreq = 0L;
bool	gbInitHW = false;

int		giAttIdxHF = 0;
int		giAttIdxVHF = 5;
int		giDefaultAttIdxHF = 2;	// 0 dB
int		giDefaultAttIdxVHF = 5;	// 0 dB
int		giMgcIdx = 0;
int		giDefaultMgcIdx = 0;	// 0 dB
int		giAgcIdx = 0;
int		giDefaultAgcIdx = 1;	// Auto
int		giMixGainIdx = 0;
int		giVGAGainIdx = 0;
int		giDefaultMixGainIdx = 0;	// Default IF gain 0
int		giDefaultVGAGainIdx = 0;    // Default VGA gain 0
int		giThrIdx = 0;
int		giDefaultThrIdx = 2;	// Threshold: 20 dB
int		giWhatIdx = 0;
int     gdownsampleratio = 1;
pfnExtIOCallback	pfnCallback = 0;

HWND Hconsole;
//Target GPIO

#define LED_OVERLOAD (1)// LED_OVERLOAD  GPIO21  bit position
#define LED_MODEA (2) 	// LED_MODEA  GPIO22
#define LED_MODEB (4) 	// LED_MODEB  GPIO22
#define SEL0 (8)  		// SEL0  GPIO26
#define SEL1 (16) 		// SEL1  GPIO27
#define SHDWN (32)  	// SHDWN  GPIO28
#define DITH (64)		// DITH   GPIO29

static bool gled_overload;
static bool gled_modea;
static bool gled_modeb;
static bool gsel0;
static bool gsel1;
static bool gshdwn;
static bool gdith;
static bool warnflag;
pthread_t adc_samples_thread, show_stats_thread;

// transfer variables
PUCHAR			*buffers;                       // export, main data buffers
PUCHAR			*contexts;
OVERLAPPED		inOvLap[QUEUE_SIZE];
float**          obuffers;

int idx;            // queue index              // export
unsigned long IDX;  // absolute index
// transfer statistic variables
double kbRead = 0;
LARGE_INTEGER StartingTime;
LARGE_INTEGER Time1, Time2;
unsigned long BytesXferred = 0;
unsigned long SamplesXIF = 0;
double kSReadIF = 0;
unsigned long Failures = 0;

// test data variable
#define RF_TABLE_SIZE (64*1024)  //virtual RF
#define IF_TABLE_SIZE (65536)
float *sineIF_table;
double ftone = 2e6;
short *sine_table_16bit;
float  *cplxIF_table;

PUSHORT			*buffersDBG;
PFLOAT			*buffloatDBG;
void *tShowStats(void *args);       // thread function
pthread_mutex_t mutexShowStats;     // unlock to show stats

void AbortXferLoop(int qidx);
void *AdcSamplesProc(void*args);
void *fake_AdcSamplesProc(void *arg);

int numcplx_samples = (EXT_BLOCKLEN);

// Dialog callback

HWND h_dialog = NULL;

SplashWindow  splashW;

#define IDD_SDDC_SETTINGS	100

int init_AdcSamplesProc(void) {

    buffers		= new PUCHAR[QUEUE_SIZE];
    contexts    = new PUCHAR[QUEUE_SIZE];
    obuffers    = new float*[QUEUE_OUT];

    if(EndPt) { // real data
        long pktSize = EndPt->MaxPktSize;
        EndPt->SetXferSize(global.transferSize);
        long ppx = global.transferSize/pktSize;
        DbgPrintf("init_main_loop, buffer transferSize = %d. packet size = %ld. packets per transfer = %ld\n"
            ,global.transferSize,pktSize,ppx);
    } else // fake data
        DbgPrintf("buffer transferSize = %d bytes.\n",global.transferSize);

    // Allocate all the buffers for the input queues
    for (int i=0; i< QUEUE_SIZE; i++) {
        buffers[i]        = new UCHAR[global.transferSize];
        inOvLap[i].hEvent = CreateEvent(NULL, false, false, NULL);
    }
    // Allocate the buffers for the output queue
    for (int i=0; i< QUEUE_OUT; i++) {
        obuffers[i]  = new float[global.transferSize/2];
    }

    // Allocate fake data sine table

    sine_table_16bit = ( short *) malloc(RF_TABLE_SIZE * sizeof( short));
    sineIF_table = ( float *) malloc(IF_TABLE_SIZE * sizeof( float));
    double pi = 4*atan(1);

    int j = 0;
    double dphi = 2.0*pi/IF_TABLE_SIZE;
    for (int i=0; i< IF_TABLE_SIZE; i++)
    {
        sineIF_table[j++] = (float) sin(i*dphi);
    }

    for (int i=0; i< RF_TABLE_SIZE; i++)
        sine_table_16bit[i] = (short) (32735.0*0.42)  *sin(i*dphi)+0.5 ;

    buffersDBG	= new PUSHORT[QUEUE_SIZE];

    ftone = 1.00e7;
    int kidx = 0, strd = round(RF_TABLE_SIZE*ftone/global.fs);

   // while( ((LONGLONG)(FAKE_TABLE_SIZE) %strd) > 0)strd--;

    int len = global.transferSize/sizeof(short);
    for (int i=0; i< QUEUE_SIZE; i++) {
        buffersDBG[i] = new USHORT[len];
        for(int n=0;n<len;n++) {
            buffersDBG[i][n] = sine_table_16bit[kidx];
            kidx += strd;
            kidx %= RF_TABLE_SIZE;
        }
    }

    return 0;
}


int start_AdcSamplesProc(void ) { // pass in the main_loopf thread function
    DbgPrintf("start_AdcSamplesProc\n");
    kbRead = 0; // zeros the kilobytes counter
    kSReadIF = 0;
    pthread_mutex_init(&mutexShowStats, NULL);
    pthread_mutex_lock(&mutexShowStats);
    global.run = true;
    int t = 0;
    pthread_create(&show_stats_thread, NULL, tShowStats, (void *)t);
    switch (gExtSampleRate)
    {
    case 32000000: gdownsampleratio = 0; break;
    case 16000000: gdownsampleratio = 1; break;
    case  8000000: gdownsampleratio = 2; break;
    case  4000000: gdownsampleratio = 3; break;
    case  2000000: gdownsampleratio = 4; break;
    }
    initR2iq(gdownsampleratio); // XPV
    global.start = true;
    if ((BBRF103.IsReady())&&((inject_tone ==  ADCstream)||
                              (inject_tone ==  ToneRF)||
                              (inject_tone ==  SweepRF)))
     pthread_create(&adc_samples_thread, NULL, AdcSamplesProc, (void *)t);
    else
    {
        if (warnflag) inject_tone = ToneIF;
        pthread_create(&adc_samples_thread, NULL, fake_AdcSamplesProc, (void *)t);
    }
    return 1; // init success
}

void *AdcSamplesProc(void*)
{
    DbgPrintf("AdcSamplesProc pthread runs\n");
    int callShowStatsRate = 32*QUEUE_SIZE;

    int odx =0;
    int rd = r2iqCntrl.getRatio();


    r2iqTurnOn(0); //XPV
    unsigned int kidx = 0, strd =0;

    if(global.start) { // start the device data stream
        // Queue-up the first batch of transfer requests
        for (int n=0; n< QUEUE_SIZE; n++) {
            contexts[n] = EndPt->BeginDataXfer(buffers[n], global.transferSize, &inOvLap[n]);
            if (EndPt->NtStatus || EndPt->UsbdStatus) {// BeginDataXfer failed
                DbgPrintf((char *)"Xfer request rejected. 1 STATUS = %ld %ld",EndPt->NtStatus,EndPt->UsbdStatus);
                pthread_exit(NULL);
            }
        }
        global.start = false;
        fx3Control(STARTFX3);       // start the firmware
        global.run = true;
        idx = 0;    // buffer cycle index
        IDX = 1;    // absolute index
        QueryPerformanceCounter(&StartingTime);  // set the start time
    }
    // The infinite xfer loop.
    while ( global.run ) {
        LONG rLen = global.transferSize;	// Reset this each time through because
        // FinishDataXfer may modify it
        if (!EndPt->WaitForXfer(&inOvLap[idx], 5000)) { // block on transfer
            EndPt->Abort(); // abort if timeout
            if (EndPt->LastError == ERROR_IO_PENDING)
                WaitForSingleObject(inOvLap[idx].hEvent, 5000);
            break;
        }
        int mdf = (Xfreq * 1024)/1000;
        if (EndPt->Attributes == 2) { // BULK Endpoint
            if (EndPt->FinishDataXfer(buffers[idx], rLen, &inOvLap[idx], contexts[idx])) {
                if(inject_tone == ToneRF)
                    {
                        short * pi = (short*) &buffers[idx][0];
                        for (unsigned int n =0; n <(global.transferSize/sizeof(short)); n++ )
                        {
                              pi[n] = sine_table_16bit[kidx];
                              kidx += mdf;
                              kidx %= RF_TABLE_SIZE;
                        }
                    }
                   // memcpy(buffers[idx],buffersDBG[idx],global.transferSize);
                if(inject_tone == SweepRF)
                    {
                        short * pi = (short*) &buffers[idx][0];
                        for (UINT n =0; n <(global.transferSize/sizeof(short)); n++ )
                        {
                              pi[n] = sine_table_16bit[kidx];
                              kidx += strd;
                              kidx %= RF_TABLE_SIZE;
                        }
                            strd +=1;
                    }
                BytesXferred += rLen;
                if(rLen < global.transferSize) printf("rLen = %ld\n",rLen);
            } else
            {
                Failures++;
                if ( pfnCallback ) pfnCallback( -1, extHw_Stop, 0.0F, 0 ); // Stop realtime
            }

        }

         if (pfnCallback && global.run)
			{
              if (rd == 1)
              {
                    pfnCallback(EXT_BLOCKLEN, 0, 0.0F, obuffers[idx]);
                    SamplesXIF += EXT_BLOCKLEN;
              }
              else
              {
                  odx = (idx+1)/rd;
                  if((odx*rd) == (idx+1))
                  {
                    pfnCallback(EXT_BLOCKLEN, 0, 0.0F, obuffers[idx/rd]);
                    SamplesXIF += EXT_BLOCKLEN;
                  }
              }
			}

        r2iqDataReady();         //  XPV play the r2iq

        // Re-submit this queue element to keep the queue full
        contexts[idx] = EndPt->BeginDataXfer(buffers[idx], global.transferSize, &inOvLap[idx]);
        if (EndPt->NtStatus || EndPt->UsbdStatus) { // BeginDataXfer failed
            DbgPrintf("Xfer request rejected. 2 NTSTATUS = 0x%08X\n",(UINT)EndPt->NtStatus);
            AbortXferLoop(idx);
            break;
        }

        if ((IDX%callShowStatsRate)==0) { //Only update the display at the call rate
            pthread_mutex_unlock(&mutexShowStats);
            if ( !global.run ) break;
        }

        ++idx;
        ++IDX;
        idx = idx%QUEUE_SIZE;
    }  // End of the infinite loop

 //   DbgPrintf("FX3Reset.\n");
 //   fx3FWControl(FX3Reset); // reset the firmware
    fx3Control(STOPFX3);
    Sleep(400);
    pthread_mutex_unlock(&mutexShowStats); //  allows exit of
    DbgPrintf("AdcSamplesProc pthread_exit\n");
    pthread_exit(NULL);
    return 0;  // void *
}

void AbortXferLoop(int qidx)
{
    long len = global.transferSize;
    bool r = true;
    EndPt->Abort();
    for (int n=0; n< QUEUE_SIZE; n++) {
   //   if (n< qidx+1)
        {
          EndPt->WaitForXfer(&inOvLap[n], TIMEOUT);
          EndPt->FinishDataXfer(buffers[n],len, &inOvLap[n],contexts[n]);
        }
        r = CloseHandle(inOvLap[n].hEvent);
        DbgPrintf("CloseHandle[%d]  %d\n", n, r);
    }
    //Deallocate all the buffers for the input queues
    delete[] buffers;
    delete[] contexts;

}


void *fake_AdcSamplesProc(void *arg) {

	unsigned LocalSampleRate;

    int callShowStatsRate = 32 * QUEUE_SIZE;

	QueryPerformanceCounter(&Time1);
	unsigned long generatorCount = 0;
    float samplesFlt[ EXT_BLOCKLEN * 2 ];
    LocalSampleRate = gExtSampleRate;
    unsigned i, j = 0;
    double time1,time2,elapsedms;
    double phase = 0.0;
    double phaseInc  =  2.0 * 3.1415926535897932384626433832795/8;
    UINT64 uphase = 0;
    UINT64 uphaseInc = 0;
    UINT16 uphs, uphc;
    switch(inject_tone)
    {
    case ToneIF:
        {
            for ( i = j = 0; i < EXT_BLOCKLEN; ++i )
            {
                samplesFlt[ j++ ] = (float)(  cos( phase ) );
                samplesFlt[ j++ ] = (float)(  sin( phase ) );
                phase += phaseInc;
                if      ( phase >  3.1415926535897932384626433832795 )	phase -= 2.0 * 3.1415926535897932384626433832795;
                else if ( phase < -3.1415926535897932384626433832795 )	phase += 2.0 * 3.1415926535897932384626433832795;
            }
        }
        break;
    default:
            memset(samplesFlt,0,sizeof(samplesFlt ));
        break;
    }

    if(global.start) { // start the device data stream
        global.start = false;
        global.run = true;
        idx = 0;    // buffer cycle index
        IDX = 0;    // absolute index
        QueryPerformanceCounter(&StartingTime);  // set the start time
        Time1 = StartingTime;
    }

	while ( global.run )
	{
		SleepEx( 1, FALSE );
		if ( !global.run )
			break;

        QueryPerformanceCounter(&Time2);
        time1 = double(Time1.QuadPart)*count2msec;
        time2 = double(Time2.QuadPart)*count2msec;
        elapsedms = time2-time1;

		generatorCount += (ULONG) ( (LocalSampleRate ) * elapsedms + 500L ) / 1000L;

		while ( generatorCount >= EXT_BLOCKLEN && global.run )
		{
			generatorCount -= EXT_BLOCKLEN;
			if (pfnCallback && global.run)
			{
              pfnCallback(EXT_BLOCKLEN, 0, 0.0F, &samplesFlt[0]);
 			  SamplesXIF += EXT_BLOCKLEN;
 			  if (inject_tone == SweepIF)
 			  {
                for ( i = j = 0; i < EXT_BLOCKLEN; ++i )
                {
                    uphs = (uphase >> 30);
                    uphc = uphs -16384 ;
                    samplesFlt[ j++ ] = sineIF_table[ uphs ];
                    samplesFlt[ j++ ] = sineIF_table[ uphc ];
                    uphase += uphaseInc;
                    uphaseInc +=  Xfreq ;
                }
 			  }
			}
		}
		Time1 = Time2;
		IDX += 4;
        if ((IDX%callShowStatsRate) == 0) {
            pthread_mutex_unlock(&mutexShowStats);
        }
	}
    pthread_mutex_unlock(&mutexShowStats); //  allows exit of
    pthread_exit(NULL);
    return 0;  // void *
}



void * tShowStats(void *args)
{
   LARGE_INTEGER EndingTime;
 //   double timeElapsed = 0;
    BytesXferred = 0;
    SamplesXIF = 0;
    char lbuffer[64];
    UINT8 cnt=0;
    while(global.run) {
        pthread_mutex_lock(&mutexShowStats);
        if(global.run == false)
            pthread_exit(NULL);

        double timeStart = double(StartingTime.QuadPart)*count2sec;
        QueryPerformanceCounter(&EndingTime);
        double timeElapsed = double(EndingTime.QuadPart)*count2sec - timeStart;
        kbRead += double(BytesXferred)/1000.;
        double mBps = (double)kbRead/timeElapsed/1000;
        kSReadIF += double(SamplesXIF)/1000.;
        double mSpsIF = (double)kSReadIF/timeElapsed/1000;

        if(!BBRF103.IsReady()) {
            DbgPrintf("BBRF103 not connected, ");
        }
        switch(inject_tone)
        {
        case ToneRF:
             DbgPrintf("I&Q %6.3f Msps, virtual RF tone\n", mSpsIF);
             break;
        case ToneIF:
             DbgPrintf("I&Q %6.3f Msps, IF Inject tone\n", mSpsIF);
             break;
        case SweepRF:
             DbgPrintf("I&Q %6.3f Msps, virtual RF sweep\n", mSpsIF);
             break;
        case SweepIF:
             DbgPrintf("I&Q %6.3f Msps, virtual IF sweep\n", mSpsIF);
             break;
        case ADCstream:
             DbgPrintf("ADC rate %6.3f Msps \n", mBps/2.0);
             break;
        default:
             break;
        }
        BytesXferred = 0;
        SamplesXIF = 0;
        if (BBRF103.ledY)
        {
            BBRF103.ledY= false;
            BBRF103.UptLedYellow(BBRF103.ledY);  //blink if thread runs gadgets BBRF103
        }
        sprintf(lbuffer,"%6.3f Msps real",mBps/2.0);
        SetWindowText(GetDlgItem(h_dialog, IDC_STATIC14),lbuffer);
        sprintf(lbuffer,"%6.3f Msps complex",mSpsIF);
        SetWindowText(GetDlgItem(h_dialog, IDC_STATIC16),lbuffer);

        if(BBRF103.GetTrace()) // debug
        {
             cnt++;
             cnt &= 0x7F;
             DbgPrintf("cnt 0x%02X\n",cnt);
//             BBRF103.SendI2cbyte(SI5351_ADDR, 22, cnt);
//             R820T2.i2c_write(0x1E,&cnt, 1);
        }
    }
    pthread_exit(NULL);  /* terminate the thread */
    return 0;
}



//---------------------------------------------------------------------------

static void stopThread()
{
	if ( global.run )
	{
        fftwf_export_wisdom_to_filename("wisdom");
		global.run = false;
		global.start = false;
        pthread_join( adc_samples_thread,NULL);
        pthread_join( show_stats_thread,NULL);
	}
}

static void startThread()
{
    fftwf_import_wisdom_from_filename("wisdom");
    start_AdcSamplesProc();
}

HMODULE hInst;
//---------------------------------------------------------------------------

extern "C"
BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
					 )
{

	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
       hInst = hModule;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

//---------------------------------------------------------------------------
extern "C"
bool __declspec(dllexport) __stdcall InitHW(char *name, char *model, int& type)
{
	type = gHwType;
	strcpy(name,  HWNAME);
	strcpy(model, HWMODEL);


	if ( !gbInitHW )
	{
		// do initialization
        warnflag = true;
		glLOfreq = 16000000L;	// just a default value
		// .......... init here the hardware controlled by the DLL
		// ......... init here the DLL graphical interface, if any
        h_dialog = CreateDialog(hInst, MAKEINTRESOURCE(IDD_DLG_MAIN), NULL, (DLGPROC)&DlgMainFn);
 //       SetWindowLong(h_dialog, GWL_STYLE, 0); //remove all window styles, check MSDN for details
/*
LONG lStyle = GetWindowLong(h_dialog, GWL_STYLE);
lStyle &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZE | WS_SYSMENU);
lStyle |= ( WS_POPUP );
SetWindowLong(h_dialog, GWL_STYLE, lStyle);
*/
        RECT rect;
        GetWindowRect(h_dialog, &rect);
        SetWindowPos(h_dialog, HWND_TOPMOST, 0, 24, rect.right-rect.left, rect.bottom-rect.top, SWP_HIDEWINDOW);
        splashW.createSplashWindow(hInst,IDB_BITMAP2,15,15,15);

      // SetWindowPos(h_dialog, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
		giAttIdxHF = giDefaultAttIdxHF;
        giAttIdxVHF = giDefaultAttIdxVHF;
		giMgcIdx = giDefaultMgcIdx;
		giAgcIdx = giDefaultAgcIdx;
		giThrIdx = giDefaultThrIdx;

#ifndef NDEBUG
        if (AllocConsole())
		{
			freopen( "CONOUT$", "wt", stdout);
			SetConsoleTitle(TEXT("Debug Black Box Console ExtIO_sspeed " HWNAME));
			Hconsole = GetConsoleWindow();
            RECT rect;
            GetWindowRect(GetDesktopWindow(), &rect);
			SetWindowPos(Hconsole, HWND_TOPMOST, rect.right-800, 24, 800, 420, SWP_SHOWWINDOW);
			DbgPrintf((char *) "Oscar Steila IK1XPV fecit MMXVIII\n");
			MakeWindowTransparent(Hconsole, 0xC0);
		}
#endif
		//		init GPIO status

		gled_overload = 0;
		gled_modea = 1;
		gled_modeb = 1;
        gsel0 = 0;
        gsel1 = 1;
		gshdwn = 0;
		gdith = 0;

		gbInitHW = true; // in case it runs offline
 //       init_AdcSamplesProc();  // allocate buffers etc ??
	}
	return gbInitHW;
}
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
extern "C"
bool EXTIO_API OpenHW(void)
{
	// .... display here the DLL panel ,if any....

//    ShowWindow(h_dialog, SW_SHOW);
//     ShowWindow(h_dialog, SW_HIDE);
#ifdef NDEBUG
    splashW.showWindow();
#endif

	if ( pfnCallback )
		pfnCallback( -1, extHw_Changed_ATT, 0.0F, 0 );

	// in the above statement, F->handle is the window handle of the panel displayed
	// by the DLL, if such a panel exists
	return gbInitHW;
}

//---------------------------------------------------------------------------
extern "C"
int  EXTIO_API StartHW(long LOfreq)
{
	int64_t ret = StartHW64( (int64_t)LOfreq );
	return (int)ret;
}

//---------------------------------------------------------------------------
extern "C"
int64_t EXTIO_API StartHW64(int64_t LOfreq)
{
    DbgPrintf((char *)"\nStartHW \n");
	if (!gbInitHW)
		return 0;

	stopThread();

    bool r = BBRF103.Init();
    if(!r)
    {
        DbgPrintf((char *) "BBRF103 not found! \n");
    }
    else
        DbgPrintf((char *) "BBRF103 initialized \n");
    init_AdcSamplesProc();  // allocate buffers...
    SetHWLO64(LOfreq);
    SetAttenuator(giAttIdxHF);
 	startThread();
    splashW.destroySplashWindow();

    SetWindowPos(h_dialog, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ShowWindow(h_dialog, SW_SHOW);
    if (BBRF103.IsReady())
    {
        SetWindowText (h_dialog, "BBRF103 connected");
        if ((glLOfreq < 32000000)||(BBRF103.RT820T2alive == false))
            {    // initialize attenuators
            BBRF103.UpdatemodeRF(HFMODE);
                SetAttenuator(giAttIdxHF);
            }
        else{
            if (BBRF103.RT820T2alive == true)
                {
                    BBRF103.UpdatemodeRF(VHFMODE);
                    SetAttenuator(giAttIdxVHF);
                    gExtSampleRate = 8000000;
                    if (pfnCallback) EXTIO_STATUS_CHANGE(pfnCallback, extHw_Changed_SampleRate  );
                }
            }
    }
    else
    {
        SetWindowText (h_dialog, "NO SDR hardware found");

        if (warnflag)
        {
           MessageBox(NULL, "SDR runs offline with virtual signals in demo mode", "WARNING BBRF103 not found", MB_OK |MB_ICONWARNING);
           warnflag = false;
        }

    }

	// number of complex elements returned each
	// invocation of the callback routine
    int64_t rx  = numcplx_samples; // /pow(2,gdownsampleratio);
	return rx;
}

//---------------------------------------------------------------------------
extern "C"
void EXTIO_API StopHW(void)
{
	stopThread();
	return;
}

//---------------------------------------------------------------------------
extern "C"
void EXTIO_API CloseHW(void)
{
	// ..... here you can shutdown your graphical interface, if any............
	if (h_dialog != NULL)
        DestroyWindow(h_dialog);

	if (gbInitHW )
	{
        BBRF103.Close();
	}
	gbInitHW = false;
}
/*
ShowGUI
void __stdcall __declspec(dllexport) ShowGUI(void)
This entry point is used by Winrad to tell the DLL that the user did ask to see the GUI of the DLL itself, if it has one.
The implementation of this call is optional 
It has  no return value.
*/
extern "C"
void EXTIO_API ShowGUI()
{
	DbgPrintf((char *) "ShowGUI\n");
	ShowWindow(h_dialog, SW_SHOW);
	SetForegroundWindow(h_dialog);
	return;
}

/*
HideGUI
void __stdcall __declspec(dllexport) HideGUI(void)
This entry point is used by Winrad to tell the DLL that it has to hide its GUI, if it has one. The implementation of
this call is optional 
It has  no return value
*/
extern "C"
void EXTIO_API HideGUI()
{
	DbgPrintf((char *) "HideGUI\n");
	ShowWindow(h_dialog, SW_HIDE);
	return;
}

extern "C"
void EXTIO_API SwitchGUI()
{
	if (IsWindowVisible(h_dialog))
    {
        DbgPrintf((char *) "SwitchGUI  HIDE\n");
        ShowWindow(h_dialog, SW_HIDE);

    }
    else
    {
         DbgPrintf((char *) "SwitchGUI  SHOW\n");
        ShowWindow(h_dialog, SW_SHOW);
    }
	return;
}

//---------------------------------------------------------------------------
extern "C"
int  EXTIO_API SetHWLO(long LOfreq)
{
	int64_t ret = SetHWLO64( (int64_t)LOfreq );
	return (ret & 0xFFFFFFFF);
}

extern "C"
int64_t EXTIO_API SetHWLO64(int64_t LOfreq)
{
	// ..... set here the LO frequency in the controlled hardware
	// Set here the frequency of the controlled hardware to LOfreq
	const int64_t wishedLO = LOfreq;
	int64_t ret = 0;

    LOfreq = r2iqCntrl.UptTuneFrq(LOfreq); //update
	// take frequency
	glLOfreq = LOfreq;

    rf_mode rfmode = BBRF103.GetmodeRF();
	if ((LOfreq > 32000000)&&( rfmode == HFMODE))
    {
        if (BBRF103.RT820T2alive == true)
        {
            BBRF103.UpdatemodeRF(VHFMODE);
            gExtSampleRate = 8000000;
            if (pfnCallback) EXTIO_STATUS_CHANGE(pfnCallback, extHw_Changed_SampleRate  );
        }
    }
	if ((LOfreq < 31000000)&&( rfmode == VHFMODE))
    {
        BBRF103.UpdatemodeRF(HFMODE);
    }
	if ( wishedLO != LOfreq  &&  pfnCallback )
    {
		EXTIO_STATUS_CHANGE(pfnCallback, extHw_Changed_LO );
    }

    if(BBRF103.GetmodeRF() == VHFMODE)
            R820T2.set_freq((UINT32) LOfreq);
	// 0 The function did complete without errors.
	// < 0 (a negative number N)
	//     The specified frequency  is  lower than the minimum that the hardware  is capable to generate.
	//     The absolute value of N indicates what is the minimum supported by the HW.
	// > 0 (a positive number N) The specified frequency is greater than the maximum that the hardware
	//     is capable to generate.
	//     The value of N indicates what is the maximum supported by the HW.
	return ret;
}


//---------------------------------------------------------------------------
extern "C"
int  EXTIO_API GetStatus(void)
{
	return 0;  // status not supported by this specific HW,
}

//---------------------------------------------------------------------------
extern "C"
void EXTIO_API SetCallback( pfnExtIOCallback funcptr )
{
	pfnCallback = funcptr;
	return;
}


//---------------------------------------------------------------------------
extern "C"
long EXTIO_API GetHWLO(void)
{
	return (long)( glLOfreq & 0xFFFFFFFF );
}

extern "C"
int64_t EXTIO_API GetHWLO64(void)
{
    if ((glLOfreq == 0)&&(BBRF103.GetmodeRF()== HFMODE))
        {
            glLOfreq = gExtSampleRate/2;
        }
    if (BBRF103.GetmodeRF()== VLFMODE)
        glLOfreq = 0;
        // glLOfreq = gExtSampleRate/2;
    else
        if (gExtSampleRate == 32000000)   // case 32 Msps sampling rate LO si fixed to 16M
           glLOfreq = 16000000;


	return glLOfreq;
}

//---------------------------------------------------------------------------
extern "C"
long EXTIO_API GetHWSR(void)
{
	// This DLL controls just an oscillator, not a digitizer
	return gExtSampleRate;
}


//---------------------------------------------------------------------------

// extern "C" long EXTIO_API GetTune(void);
// extern "C" void EXTIO_API GetFilters(int& loCut, int& hiCut, int& pitch);
// extern "C" char EXTIO_API GetMode(void);
// extern "C" void EXTIO_API ModeChanged(char mode);
// extern "C" void EXTIO_API IFLimitsChanged(long low, long high);
// extern "C" void EXTIO_API TuneChanged(long freq);

// extern "C" void    EXTIO_API TuneChanged64(int64_t freq);
// extern "C" int64_t EXTIO_API GetTune64(void);
// extern "C" void    EXTIO_API IFLimitsChanged64(int64_t low, int64_t high);

//---------------------------------------------------------------------------

// extern "C" void EXTIO_API RawDataReady(long samplerate, int *Ldata, int *Rdata, int numsamples)

//---------------------------------------------------------------------------
extern "C"
void EXTIO_API VersionInfo(const char * progname, int ver_major, int ver_minor)
{
  SDR_progname[0] = 0;
  SDR_ver_major = -1;
  SDR_ver_minor = -1;

  if ( progname )
  {
    strncpy( SDR_progname, progname, sizeof(SDR_progname) -1 );
    SDR_ver_major = ver_major;
    SDR_ver_minor = ver_minor;

	// possibility to check program's capabilities
	// depending on SDR program name and version,
	// f.e. if specific extHWstatusT enums are supported
  }
}

//---------------------------------------------------------------------------

// following "Attenuator"s visible on "RF" button
int  GetAttenuatorsHF(int atten_idx, float * attenuation)
{
	// fill in attenuation
	// use positive attenuation levels if signal is amplified (LNA)
	// use negative attenuation levels if signal is attenuated
	// sort by attenuation: use idx 0 for highest attenuation / most damping
	// this functions is called with incrementing idx
	//    - until this functions return != 0 for no more attenuator setting

		switch (atten_idx)
		{
		case 0:		*attenuation = -20.0F;	return 0;
		case 1:		*attenuation = -10.0F;	return 0;
		case 2:		*attenuation = 0.0F;	return 0;
		default:	return 1;
		}
}

int  GetAttenuatorsVHF(int atten_idx, float * attenuation)
{
	// fill in attenuation
	// use positive attenuation levels if signal is amplified (LNA)
	// use negative attenuation levels if signal is attenuated
	// sort by attenuation: use idx 0 for highest attenuation / most damping
	// this functions is called with incrementing idx
	//    - until this functions return != 0 for no more attenuator setting
	if ((atten_idx >= 0) && (atten_idx < 15))  // 15 steps
	{
		*attenuation =  (float) r820t2_lna_gain_steps[atten_idx];
		return 0;
	}
	return 1;
}


extern "C"
int EXTIO_API GetAttenuators( int atten_idx, float * attenuation )
{
	// fill in attenuation
	// use positive attenuation levels if signal is amplified (LNA)
	// use negative attenuation levels if signal is attenuated
	// sort by attenuation: use idx 0 for highest attenuation / most damping
	// this functions is called with incrementing idx
	//    - until this functions return != 0 for no more attenuator setting
	if (BBRF103.GetmodeRF() == VHFMODE)
		return GetAttenuatorsVHF( atten_idx, attenuation);
	else                     // VLFMODE HFMODE
		return GetAttenuatorsHF( atten_idx, attenuation);
}


extern "C"
int EXTIO_API GetActualAttIdx(void)
{
    if (BBRF103.GetmodeRF() == VHFMODE)
        return giAttIdxVHF;	// returns -1 on error
    else
        return giAttIdxHF;	// returns -1 on error
}

extern "C"
int EXTIO_API SetAttenuator( int atten_idx )
{
    rf_mode rfmode = BBRF103.GetmodeRF();
    if (rfmode== VHFMODE)
    {
      	if ((atten_idx >=0) && (atten_idx < 15))  // 15 steps 0-14
		{
			R820T2.set_lna_gain(atten_idx);
			giAttIdxVHF = atten_idx;
		}
    }
    else
    {
        BBRF103.UpdateattRF(atten_idx);
        giAttIdxHF = atten_idx;
    }
    return 0; // no error
}


//---------------------------------------------------------------------------

// optional function to get AGC Mode: AGC_OFF (always agc_index = 0), AGC_SLOW, AGC_MEDIUM, AGC_FAST, ...
// this functions is called with incrementing idx
//    - until this functions returns != 0, which means that all agc modes are already delivered

extern "C"
int EXTIO_API ExtIoGetAGCs(int agc_idx, char * text)	// text limited to max 16 char
{
    if (BBRF103.GetmodeRF()== VHFMODE)
    {
        switch (agc_idx)
        {
            case 0:		strcpy(text, "Mix");	return 0;  // shows "IF"
            case 1:		strcpy(text, "VGA");	return 0;
            default:	return 1;
        }
    }
	return 1;
}

extern "C"
int EXTIO_API ExtIoGetActualAGCidx(void)
{
	return giAgcIdx;	// returns -1 on error
}

extern "C"
int EXTIO_API ExtIoSetAGC(int agc_idx)
{
	int iPrevAgcIdx = giAgcIdx;
	switch (agc_idx)
	{
	case 0:
	case 1:
		giAgcIdx = agc_idx;
		if (iPrevAgcIdx != giAgcIdx)
		{
			if (pfnCallback)
				EXTIO_STATUS_CHANGE(pfnCallback, extHw_Changed_RF_IF);
		}
		return 0;
	default:
		return 1;	// ERROR
	}
	return 1;	// ERROR
}

// optional: HDSDR >= 2.62
extern "C"
int EXTIO_API ExtIoShowMGC(int agc_idx)		// return 1, to continue showing MGC slider on AGC
											// return 0, is default for not showing MGC slider
{
    if (BBRF103.GetmodeRF()== VHFMODE)
    {
        switch (agc_idx)
        {
        case 0:	return 1;	// MiX (IF)
        case 1:	return 1;	// VGA
        default:
            return 0;	// ERROR
        }
    }
	return 0;	// ERROR
}
//---------------------------------------------------------------------------

// following "MGC"s visible on "IF" button

extern "C"
int EXTIO_API ExtIoGetMGCs(int mgc_idx, float * gain)
{
	// fill in gain
	// sort by ascending gain: use idx 0 for lowest gain
	// this functions is called with incrementing idx
	//    - until this functions returns != 0, which means that all gains are already delivered
    if (BBRF103.GetmodeRF()== VHFMODE)
    {
        switch (giAgcIdx)
        {
        case 0:	// Mix  (IF)
            if ((mgc_idx >= 0) && (mgc_idx < 16))
            {
                *gain = (float) mgc_idx ;	return 0;
            }else
                return 1;
            break;
        case 1:	// VGA
            if ((mgc_idx >= 0) && (mgc_idx < 16))
            {
                *gain = (float)mgc_idx*3.0F;	return 0;
            }
            else
                return 1;
            break;
        }
    }
	return 1;
}

extern "C"
int EXTIO_API ExtIoGetActualMgcIdx(void)
{
	switch (giAgcIdx)
	{
	case 0:	// Mix
		return giMixGainIdx;
	case 1:	// VGA
		return giVGAGainIdx;
	}
	return -1;
}

extern "C"
int EXTIO_API ExtIoSetMGC(int mgc_idx)
{
//	int iPrevMgcIdx = giMgcIdx;
	int iPrevMixIdx = giMixGainIdx;
	int iPrevVGAIdx = giVGAGainIdx;
	//
//	double x = 0.0F;
	switch (giAgcIdx)
	{
	case 0:	// Mix
		if ((mgc_idx >= 0) && (mgc_idx < 16))
		{
			giMixGainIdx = mgc_idx;
			if (iPrevMixIdx != giMixGainIdx)
			{
				if (pfnCallback)
					EXTIO_STATUS_CHANGE(pfnCallback, extHw_Changed_RF_IF);
				if (BBRF103.GetmodeRF() == VHFMODE)
					R820T2.set_mixer_gain(giMixGainIdx);
			}
			return 0;
		}
		else
			return 1;	// ERROR
		break;

	case 1:	// VGA
		if ((mgc_idx >= 0) && (mgc_idx < 6))
		{
			giVGAGainIdx = mgc_idx;
			if (iPrevVGAIdx != giVGAGainIdx)
			{
				if (pfnCallback)
					EXTIO_STATUS_CHANGE(pfnCallback, extHw_Changed_RF_IF);
				if (BBRF103.GetmodeRF() == VHFMODE)
                {
   					R820T2.set_vga_gain(giVGAGainIdx);
   					Sleep(30);
                }
			}
			return 0;
		}
		else
			return 1;	// ERROR
		break;
	}
	return 1;	// ERROR
}

//---------------------------------------------------------------------------
// fill in possible samplerates
extern "C"
int EXTIO_API ExtIoGetSrates( int srate_idx, double * samplerate )
{
    switch ( srate_idx )
    {
    case 0:		*samplerate =  2000000.0;	return 0;
    case 1:		*samplerate =  4000000.0;	return 0;
    case 2:		*samplerate =  8000000.0;	return 0;
    case 3:		*samplerate = 16000000.0;	return 0;
    case 4:		*samplerate = 32000000.0;	return 0;
    }
	return 1;	// ERROR
}

extern "C"
int  EXTIO_API ExtIoGetActualSrateIdx(void)
{
    	return giExtSrateIdx;
}

extern "C"
int  EXTIO_API ExtIoSetSrate( int srate_idx )
{
	double newSrate = 0.0;
	if ( 0 == ExtIoGetSrates( srate_idx, &newSrate ) )
	{
		giExtSrateIdx = srate_idx;
		gExtSampleRate = (unsigned)( newSrate + 0.5 );
        switch (gExtSampleRate)
        {
        case 32000000: gdownsampleratio = 0; break;
        case 16000000: gdownsampleratio = 1; break;
        case  8000000: gdownsampleratio = 2; break;
        case  4000000: gdownsampleratio = 3; break;
        case  2000000: gdownsampleratio = 4; break;
        }
        r2iqCntrl.Setdecimate(gdownsampleratio);
        int64_t newLOfreq = r2iqCntrl.UptTuneFrq(glLOfreq);
        if ( newLOfreq != glLOfreq)
        {
            glLOfreq = newLOfreq;
            if (pfnCallback)
            {
                pfnCallback( -1, extHw_Changed_LO, 0.0F, 0 );
            }
        }
		return 0;
	}
	return 1;	// ERROR
}

extern "C"
long EXTIO_API ExtIoGetBandwidth( int srate_idx )
{
	double newSrate = 0.0;
	long ret = -1L;
	if ( 0 == ExtIoGetSrates( srate_idx, &newSrate ) )
	{
		switch ( srate_idx )
		{
			case 0:		ret = 800000L;	break;
			case 1:		ret = 1800000L;	break;
			case 2:		ret = 3800000L;	break;
			case 3:		ret = 7800000L;	break;
			case 4:		ret = 15700000L;	break;
			default:	ret = -1L;		break;
		}
		return ( ret >= newSrate || ret <= 0L ) ? -1L : ret;
	}
	return -1L;	// ERROR
}

//---------------------------------------------------------------------------
// will be called (at least) before exiting application
extern "C"
int  EXTIO_API ExtIoGetSetting( int idx, char * description, char * value )
{
	switch ( idx )
	{
	case 0: snprintf( description, 1024, "%s", "Identifier" );		snprintf( value, 1024, "%s", SETTINGS_IDENTIFIER );	return 0;
	case 1:	snprintf( description, 1024, "%s", "SampleRateIdx" );	snprintf( value, 1024, "%d", giExtSrateIdx );		return 0;
	case 2:	snprintf( description, 1024, "%s", "AttenuationIdxHF" );	snprintf( value, 1024, "%d", giAttIdxHF );			return 0;
	case 3:	snprintf( description, 1024, "%s", "AttenuationIdxVHF" );   snprintf( value, 1024, "%d", giAttIdxVHF );			return 0;
	default:	return -1;	// ERROR
	}
	return -1;	// ERROR
}


extern "C"
void EXTIO_API ExtIoSetSetting( int idx, const char * value )
{
	double newSrate;
	float  newAtten = 0.0F;
	int tempInt;
	// now we know that there's no need to save our settings into some (.ini) file,
	// what won't be possible without admin rights!!!,
	// if the program (and ExtIO) is installed in C:\Program files\..
	SDR_supports_settings = true;
	if ( idx != 0 && !SDR_settings_valid )
		return;	// ignore settings for some other ExtIO

	switch ( idx )
	{
	case 0:		SDR_settings_valid = ( value && !strcmp( value, SETTINGS_IDENTIFIER ) );
				// settings are version specific !
				break;
	case 1:		tempInt = atoi( value );
				if ( 0 == ExtIoGetSrates( tempInt, &newSrate ) )
				{
					giExtSrateIdx = tempInt;
					gExtSampleRate = (unsigned)( newSrate + 0.5 );
				}
				break;
	case 2:		tempInt = atoi( value );
				if ( 0 == GetAttenuators( tempInt,&newAtten ) )
					giDefaultAttIdxHF = tempInt;
				break;
    case 3:		tempInt = atoi( value );
				if ( 0 == GetAttenuators( tempInt,&newAtten ) )
					giDefaultAttIdxVHF =  tempInt;
				break;

	}

}


//---------------------------------------------------------------------------
