# ExtIO_sddc-Ver0.96
ExtIO_sddc.dll project links to pthreads, fftw, CyAPI. IDE Code::BLocks 

ExtIO.dll Software Digital Down Converter for BBRF103 by Oscar Steila ik1xpv
Version "SDDC-0.96" 25/02/2018.
It requires:
	CodeBlocks 12.11 IDE or later,  https://sourceforge.net/projects/codeblocks/files/Binaries/12.11/Windows/ 
	
	pthreads   https://sourceforge.net/projects/pthreads4w/files/pthreads-w32-2-9-1-release.zip/download
				( copy  Pre-built.2 directory contens into /lib/pthreads/ )
	fftw libraries ftp://ftp.fftw.org/pub/fftw/fftw-3.3.5-dll32.zip
				( copy fftw-3.3.5-dll32.zip contens to /lib/fftw )
	CyAPI.cpp library source can be downloaded at http://www.cypress.com/file/289981/download (see License)
				( directory lib\CyAPI_gcc contains the CodeBlocks project to compile under gcc)
	
Directory structure:

	\Source\ 		> ExtIO_sddc sources + ExtIO_sddc.cbp (CodeBlocks Project),
	\Lib\fftw   	> fftw library here,
		\pthreads	> pthreads library here,
		\CyAPI_gcc	> CyAPI gcc library here,
	\bin\debug		> debug, 
		\release	> release.
		
links and reference:
   http://www.steila.com/blog/
   http://www.hdsdr.de/
   http://booyasdr.sourceforge.net/
   http://www.cypress.com/ 
