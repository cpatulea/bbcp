/******************************************************************************/
/*                                                                            */
/*                           b b c p _ N o d e . C                            */
/*                                                                            */
/*                                                                            */
/*(c) 2002-17 by the Board of Trustees of the Leland Stanford, Jr., University*/
/*      All Rights Reserved. See bbcp_Version.C for complete License Terms    */
/*   Produced by Andrew Hanushevsky for Stanford University under contract    */
/*              DE-AC02-76-SFO0515 with the Department of Energy              */
/*                                                                            */
/* bbcp is free software: you can redistribute it and/or modify it under      */
/* the terms of the GNU Lesser General Public License as published by the     */
/* Free Software Foundation, either version 3 of the License, or (at your     */
/* option) any later version.                                                 */
/*                                                                            */
/* bbcp is distributed in the hope that it will be useful, but WITHOUT        */
/* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or      */
/* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public       */
/* License for more details.                                                  */
/*                                                                            */
/* You should have received a copy of the GNU Lesser General Public License   */
/* along with bbcp in a file called COPYING.LESSER (LGPL license) and file    */
/* COPYING (GPL license).  If not, see <http://www.gnu.org/licenses/>.        */
/*                                                                            */
/* The copyright holder's institutional names and contributor's names may not */
/* be used to endorse or promote products derived from this software without  */
/* specific prior written permission of the institution or contributor.       */
/******************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include "bbcp_Config.h"
#include "bbcp_Emsg.h"
#include "bbcp_Node.h"
#include "bbcp_BuffPool.h"
#include "bbcp_Headers.h"
#include "bbcp_Network.h"
#include "bbcp_ProgMon.h"
#include "bbcp_Protocol.h"
#include "bbcp_Pthread.h"
#include "bbcp_System.h"
#include "bbcp_ZCX.h"
  
/******************************************************************************/
/*                      E x t e r n a l   O b j e c t s                       */
/******************************************************************************/
  
extern bbcp_Config   bbcp_Cfg;

extern bbcp_BuffPool bbcp_APool;
extern bbcp_BuffPool bbcp_BPool;
extern bbcp_BuffPool bbcp_CPool;

extern bbcp_Network  bbcp_Net;

extern bbcp_System   bbcp_OS;
 
/******************************************************************************/
/*            E x t e r n a l   T h r e a d   I n t e r f a c e s             */
/******************************************************************************/

extern "C"
{
void *bbcp_doCX(void *pp)
{
     bbcp_ZCX *cxp = (bbcp_ZCX *)pp;
     long retc = cxp->Process();
     return (void *)retc;
}
void *bbcp_doWrite(void *pp)
{
     bbcp_File *fp = (bbcp_File *)pp;
     long retc = fp->Write_All(bbcp_BPool, bbcp_Cfg.Streams);
     return (void *)retc;
}
void *bbcp_Buff2Net(void *link)
{
     bbcp_Link *netlink = (bbcp_Link *)link;
     long retc = netlink->Buff2Net();
     return (void *)retc;
}
void *bbcp_Net2Buff(void *link)
{
     bbcp_Link *netlink = (bbcp_Link *)link;
     long retc = netlink->Net2Buff();
     return (void *)retc;
}
void *bbcp_Connect(void *protp)
{
     bbcp_Protocol *protocol = (bbcp_Protocol *)protp;
     bbcp_Link     *link;
     int            retc;

     if ((link = bbcp_Net.Connect(bbcp_Cfg.CBhost, bbcp_Cfg.CBport)))
        {if ((retc = protocol->Login(link, 0)) < 0)
            {delete link; link = 0;}
        }
     return (void *)link;
}
}
  
/******************************************************************************/
/*                           C o n s t r u c t o r                            */
/******************************************************************************/
  
bbcp_Node::bbcp_Node(bbcp_Link *netLink)
{

// If this is a local node, do a simple setup
//
   if (!netLink)
      {nodename = 0; data_link[0] = 0; iocount = dlcount = 0; return;}

// For remote nodes, attach this node to the network link
//
   nodename = strdup(netLink->LinkName());
   NStream.Attach(netLink->FD());
   data_link[0] = netLink;
   dlcount = 1;
   comp1File = comp2File = 0;
   TotFiles = 0;
   TotBytes = 0;
}

/******************************************************************************/
/*                            g e t B u f f e r s                             */
/******************************************************************************/
  
int bbcp_Node::getBuffers(int isTrg, int isLZO)
{

// Allocate the buffers
//
   return bbcp_BPool.Allocate(bbcp_Cfg.BNum, bbcp_Cfg.RWBsz, isTrg);
}

/******************************************************************************/
/*                               G e t L i n e                                */
/******************************************************************************/

char *bbcp_Node::GetLine()
{
   char *lp = NStream.GetLine();

   DEBUG("Received from " <<nodename <<": " <<lp <<endl);

   return lp;
}
  
/******************************************************************************/
/*                                   P u t                                    */
/******************************************************************************/

int bbcp_Node::Put(char *data[], int dlen[])
{
   static bbcp_Mutex putMutex;

   if (DEBUGON)
      {int i= 0;
       cerr <<"bbcp_" <<bbcp_Debug.Who <<": Sending to " <<nodename <<": ";
       while(data[i])if (*data[i]) cerr <<data[i++];
                        else i++;
       cerr <<endl;
      }

   if (bbcp_Cfg.Options & bbcp_SRC) return NStream.Put(data, dlen);
      else {int rc;
            putMutex.Lock();
            rc = NStream.Put(data, dlen);
            putMutex.UnLock();
            return rc;
           }
}

/******************************************************************************/
/*                                   R u n                                    */
/******************************************************************************/
  
int bbcp_Node::Run(char *user, char *host, char *prog, char *parg)
{
   static char ipv4[] = {'-','4','\0'};
   int fderr, numa = 0;
   char *username, *sshDest, bufDest[264], *Argv[1024], *ap, *pp = prog;
   const int ArgvSize = sizeof(Argv)/sizeof(char *)-2;

// Free up any node name here
//
   if (nodename) free(nodename);
   nodename = strdup(host ? host : bbcp_Cfg.MyHost ? bbcp_Cfg.MyHost : "");
   username = (user ? user : bbcp_Cfg.MyUser ? bbcp_Cfg.MyUser : (char*)"");

// Check for an IPV6 address as ssh does not follow the rfc standard
//
   if (*nodename != '[') sshDest = nodename;
      else {int i = strlen(nodename);
            if (i > (int)sizeof(bufDest)) return -EHOSTUNREACH;
            strcpy(bufDest, nodename+1);
            bufDest[i-2] = 0; sshDest= bufDest;
           }

// Break up the command line and perform substitutions
//
   if (!(user || host)) {Argv[0] = bbcp_Cfg.MyProg; numa = 1;}
      else for (numa = 0; *pp && numa < ArgvSize; numa++)
               {while(*pp && *pp == ' ') pp++;
                ap = pp;
                while(*pp && *pp != ' ') pp++;
                if (*pp) {*pp = '\0'; pp++;}
                if (*ap == '%' && !ap[2])
                   {     if (ap[1] == 'I')
                            {if (bbcp_Cfg.IDfn)
                                {Argv[numa++] = (char *)"-i";
                                 Argv[numa] = bbcp_Cfg.IDfn;}
                                else numa--;
                            }
                    else if (ap[1] == 'U') Argv[numa] = username;
                    else if (ap[1] == 'H') Argv[numa] = sshDest;
                    else if (ap[1] == '4')
                            {if (bbcp_Cfg.Options & bbcp_IPV4)
                                           Argv[numa] = ipv4;
                                else numa--;
                            }
                    else                   Argv[numa] = ap;
                   }
                    else Argv[numa] = ap;
               }

// Add arguments (these may need to be tokenized as well)
//
   while(numa <= ArgvSize)
        {while(*parg == ' ') parg++;
         Argv[numa++] = parg;
         while(*parg && *parg != ' ') parg++;
         if (!(*parg)) break;
         *parg++ = 0;
        }

// Complete argument list to start the actual copy program
//
   if (numa >= ArgvSize) return bbcp_Emsg("Run", -E2BIG, "starting", prog);
   Argv[numa] = 0;

// Invoke the program
//
   if ((fderr=NStream.Exec(Argv, 1, bbcp_Cfg.MLog != 0)) < 0) return -fderr;

// Perform logging function here
//
   if (bbcp_Cfg.MLog) bbcp_Cfg.MLog->Monitor(fderr, parg);

// Perform debugging here
//
   if (DEBUGON)
      {int i;
       cerr <<"bbcp_" <<bbcp_Debug.Who <<": Running as pid " <<NStream.getPID() <<": ";
       for (i = 0; i < numa; i++) if (Argv[i]) cerr <<Argv[i] <<' ';
       cerr <<endl;
      }

// All done
//
   return 0;
}
  
/******************************************************************************/
/*                                  S t o p                                   */
/******************************************************************************/

void bbcp_Node::Stop(int Report)
{
   int i;

// If window reporting wanted do so only if very verbose and autotuning
//
   if (bbcp_Cfg.Options & bbcp_BLAB && bbcp_Net.AutoTune() && data_link[0])
      chkWsz(data_link[0]->FD(), 1);

// Kill any attached process
//
   NStream.Kill();

// Dismantle all of the data connections
//
   for (i = 0; i < dlcount; i++)
       if (data_link[i]) {delete data_link[i]; data_link[i] = 0;}
}
 
/******************************************************************************/
/*                                  W a i t                                   */
/******************************************************************************/

int bbcp_Node::Wait(bbcp_Node *other)
{
    pid_t pvec[3];
    bbcp_Stream *pstrm[2];
    int retc, i=0;

// Collect all of the nodes we will be waiting for
//
   if ((pvec[i] = NStream.getPID()) >= 0) 
      {pstrm[i] = &NStream; i++;}
   if (other && (pvec[i] = other->NStream.getPID()) >= 0)
      {pstrm[i] = &(other->NStream); i++;}
   if (!i) return 0;
   pvec[i] = 0;

// Issue wait and return the status
//
   do {retc = bbcp_OS.Waitpid(pvec, &i);
       pstrm[i]->clrPID();
       if (!retc)
          {if (!i) {pstrm[0] = pstrm[1]; pvec[0] = pvec[1];}
           pvec[1] = 0;
          }
      } while (!retc && pvec[0]);
   return retc;
}
  
/******************************************************************************/
/*                 D a t a   T r a n s f e r   M e t h o d s                  */
/******************************************************************************/
/******************************************************************************/
/*                              R e c v F i l e                               */
/******************************************************************************/

int bbcp_Node::RecvFile(bbcp_FileSpec *fp, bbcp_Node *Remote)
{
   static const int wOnly = S_IWUSR;

   const char *Args = 0, *Act = "opening", *Path = fp->targpath;
   char *tmpPath = 0;
   long tretc = 0;
   int i, oflag, retc, Mode = wOnly, progtid = 0;
   long long startoff = 0;
   pid_t Child[2] = {0,0};
   bbcp_File *outFile, *seqFile = 0;
   bbcp_ZCX *cxp = 0;
   pthread_t tid, link_tid[BBCP_MAXSTREAMS+4];
   bbcp_Timer Elapsed_Timer;
   bbcp_ProgMon *pmp = 0;
   float CRatio;

// Perform Force or Append processing
//
        if (bbcp_Cfg.Options & bbcp_XPIPE)
           {oflag = O_WRONLY;
            Path = bbcp_Cfg.snkSpec->pathname;
            Args = bbcp_Cfg.snkSpec->fileargs;
            if (bbcp_Cfg.snkSpec->Info.Otype != 'p') Act = "running";
               else {Mode |= S_IFIFO;
                     if (Args)
                        {bbcp_Fmsg("RecvFile",
                                   "Spaces disallowed in named output pipe",Path);
                         return -EINVAL;
                        }
                    }
           }
   else if (bbcp_Cfg.Options & bbcp_FORCE)
           {if (!(bbcp_Cfg.Options & bbcp_NOUNLINK))
               fp->FSys()->RM(Path);
            oflag = O_WRONLY | O_CREAT | O_TRUNC;
           }
   else if (bbcp_Cfg.Options & bbcp_APPEND)
           {if ((retc = fp->WriteSigFile())) return retc;
            if ((startoff = fp->targetsz)) oflag = O_WRONLY;
               else oflag = O_CREAT | O_WRONLY;
           }
   else    oflag = O_WRONLY | O_CREAT | O_EXCL;

// Establish mode, we normally make the file write-only
//
   if ( bbcp_Cfg.Options &  bbcp_RTCSNK
   && !(bbcp_Cfg.Options & (bbcp_RTCHIDE|bbcp_XPIPE)))
      Mode = bbcp_Cfg.Mode|S_IWUSR|S_ISUID;

// Tell the user what we are bout to do
//
   if ((bbcp_Cfg.Options & bbcp_BLAB) | bbcp_Cfg.Progint)
      if (bbcp_Cfg.Options & bbcp_APPEND) 
         {char buff[32];
          sprintf(buff, "%lld", startoff);
          bbcp_Fmsg("RecvFile","Appending to",Path,"at offset",buff);
         }
         else bbcp_Fmsg("RecvFile", "Creating", Path);
      else DEBUG("Receiving " <<fp->pathname <<" as " <<Path <<" offset=" <<startoff);

// Receive the file in a sub-process so that we don't muck with this one
//
   if ((Child[0] = bbcp_OS.Fork()) < 0)
      return bbcp_Emsg("RecvFile", errno, "forking to create", Path);
   if (Child[0]) 
      {char buff[128];
       Parent_Monitor.Start(0,Remote);
       DEBUG("Waiting for child " <<Child[0] <<" to finish");
       retc = bbcp_OS.Waitpid(Child);
       Parent_Monitor.Stop();
       if (bbcp_Cfg.Options & bbcp_BLAB)
          write(STDERR_FILENO, buff, Usage("Target", buff, sizeof(buff)));
       return retc;
      }

/*******************************************************************************
   (net)->data_link[i]->BPool->CStage[1]->CStage[0]->CPool->outFile->(file)
*******************************************************************************/

// Set Concurrency
//
   bbcp_Thread_MT(bbcp_Cfg.MTLevel);

// Request direct I/O if so wanted
//
   if (bbcp_Cfg.Options & bbcp_ODIO) {fp->FSys()->DirectIO(1);
       DEBUG("Direct output requested.");}

// Make temporary file name if specified
//
   if (bbcp_Cfg.Options & bbcp_RTTMP)
      {tmpPath = new char[strlen(Path)+1+4];
       strcpy(tmpPath,Path);
       strcat(tmpPath,"_tmp");
       Path = tmpPath;
      }

// Open the file and set the starting offset
//
   Elapsed_Timer.Start();
   if (!(outFile = fp->FSys()->Open(Path, oflag, Mode, Args)))
      return bbcp_Emsg("RecvFile", errno, Act, Path);
   if (startoff && ((retc = outFile->Seek(startoff)) < 0))
      return bbcp_Emsg("RecvFile",retc,"setting write offset for",Path);
   outFile->setSize(fp->Info.size);

// If compression is wanted, set up the compression objects
//
   if (bbcp_Cfg.Options & bbcp_COMPRESS 
   && !(cxp = setup_CX(0, outFile->ioFD()))) return -ECANCELED;

// Start a thread for each data link we have
//
   for (i = 0; i < dlcount; i++)
       {if ((retc = bbcp_Thread_Start(bbcp_Net2Buff, "bbcp_Net2Buff",
                                (void *)data_link[i], &tid))<0)
           {bbcp_Emsg("RecvFile",retc,"starting net thread for",Path);
            _exit(100);
           }
        link_tid[i] = tid;
        DEBUG("Thread " <<tid <<" assigned to stream " <<i);
       }

// If we are compressing start the sequence thread now
//
   if (bbcp_Cfg.Options & bbcp_COMPRESS)
      {seqFile = new bbcp_File(Path, 0, 0);
       if ((retc = bbcp_Thread_Start(bbcp_doWrite, "bbcp_doWrite",
                                     (void *)seqFile, &tid))<0)
          {bbcp_Emsg("RecvFile",retc,"starting disk thread for",Path);
           _exit(100);
          }
       link_tid[dlcount++] = tid;
       DEBUG("Thread " <<tid <<" assigned to sequencer as stream " <<i);
      }

// Start the parent process monitor. It is stopped at exit.
//
   Parent_Monitor.Start();

// If a periodic progress message is wanted, start a progress thread
//
   if (bbcp_Cfg.Progint) 
      {pmp = new bbcp_ProgMon();
       pmp->Start(outFile, cxp, bbcp_Cfg.Progint, fp->Info.size-startoff);
      }

// Write the whole file
//
   if (bbcp_Cfg.Options & bbcp_COMPRESS)
           retc = outFile->Write_All(bbcp_APool, 1);
      else retc = outFile->Write_All(bbcp_BPool, bbcp_Cfg.Streams);
   DEBUG("File write ended; rc=" <<retc);

// Wait for the expansion thread to end
//
   if (bbcp_Cfg.Options & bbcp_COMPRESS)
      {if ((tretc = (long)bbcp_Thread_Wait(cxp->TID))) retc = 128;
       DEBUG("File expansion ended; rc=" <<tretc);
      }

// Kill the progress monitor
//
   if (pmp) 
      {DEBUG("Deleting progress monitor");
       delete pmp;
      }

// Make sure each thread has terminated normally
//
   for (i = 0; i < dlcount; i++)
       {if ((tretc = (long)bbcp_Thread_Wait(link_tid[i]))) retc = 128;
        DEBUG("Thread " <<link_tid[i] <<" stream " <<i <<" ended; rc=" <<tretc);
       }

// Make sure that all of the bytes were transfered
//
   if (!retc && strncmp(Path, "/dev/null/", 10))
      {bbcp_FileInfo Info;
       if ((retc = fp->FSys()->Stat(Path, &Info)) < 0)
          {retc = -retc;
           bbcp_Emsg("RecvFile", retc, "finding", Path);
          }
          else if (Info.size != fp->Info.size && Info.mode
               &&  !(bbcp_Cfg.Options & bbcp_NOFSZCHK))
                  {const char *What = (Info.size < fp->Info.size
                                    ?  "Not all" : "Too much");
                   retc = 29;
                   bbcp_Fmsg("RecvFile",What,"data was transfered for",Path);
                   DEBUG("src size=" <<fp->Info.size <<" snk size=" <<Info.size);
                  }
      } DEBUG("Outfile " <<Path <<" closed");

// Rename temporary file if necessary (we don't do it if the copy failed)
//
   if (bbcp_Cfg.Options & bbcp_RTTMP)
      {if (!retc && rename(Path,fp->targpath) !=0 )
          bbcp_Emsg("RecvFile", errno, "rename", Path);
       delete[] tmpPath;
       Path=fp->targpath;
      }

// Report detailed I/O stats, if so wanted
//
   Elapsed_Timer.Stop();
   if (!retc && bbcp_Cfg.Options & bbcp_VERBOSE)
      {double ttime;
       Elapsed_Timer.Report(ttime);
       Report(ttime, fp, outFile, cxp);
      }

// All done
//
   Parent_Monitor.Stop();
                delete outFile;
   if (cxp)     delete(cxp);
   if (seqFile) delete(seqFile);
   retc = fp->Finalize(retc);
   close(1); close(2);
   DEBUG("Process " <<getpid() <<" exiting with rc=" <<retc);
   exit(retc);
   return(retc);  // some compilers insist on a return in int functions
}

/******************************************************************************/
/*                              S e n d F i l e                               */
/******************************************************************************/

int bbcp_Node::SendFile(bbcp_FileSpec *fp)
{
   const char *Act = "opening";
   int i, retc, tretc = 0, Mode = 0;
   pid_t Child[2] = {0,0};
   bbcp_File *inFile;
   bbcp_ProcMon *TLimit = 0;
   bbcp_ZCX *cxp;
   pthread_t tid, link_tid[BBCP_MAXSTREAMS+1];

// Set open options (check for pipes)
//
   if (bbcp_Cfg.Options & bbcp_XPIPE)
      {if (fp->Info.Otype == 'p') Mode = S_IFIFO;
          else Act = "running";
      }

// Send the file in a sub-process so that we don't muck with this one
//
   DEBUG("Sending file " <<fp->targpath <<"; offset=" <<fp->targetsz);
   if ((Child[0] = bbcp_OS.Fork()) < 0)
      return bbcp_Emsg("SendFile", errno, "forking to send", fp->pathname);
   if (Child[0])
      {char buff[128];
       Parent_Monitor.Start();
       retc = bbcp_OS.Waitpid(Child);
       Parent_Monitor.Stop();
       if (bbcp_Cfg.Options & bbcp_BLAB)
          write(STDERR_FILENO, buff, Usage("Source", buff, sizeof(buff)));
       return retc;
      }

/*******************************************************************************
    (file)->inFile->CPool->CStage[0]->CStage[1]->BPool->data_link[i]->(net)
*******************************************************************************/

// Set Concurrency
//
   bbcp_Thread_MT(bbcp_Cfg.MTLevel);

// Request direct I/O if so wanted
//
   if (bbcp_Cfg.Options & bbcp_IDIO) {fp->FSys()->DirectIO(1);
       DEBUG("Direct input requested.");}

// Open the input file and set starting offset
//
   if (!(inFile = fp->FSys()->Open(fp->pathname,O_RDONLY,Mode,fp->fileargs)))
      {bbcp_Emsg("SendFile", errno, Act, fp->pathname);
       exit(2);
      }
   if (fp->targetsz && ((retc = inFile->Seek(fp->targetsz)) < 0))
      return bbcp_Emsg("SendFile",retc,"setting read offset for",fp->pathname);

// If compression is wanted, set up the compression objects
//
   if (bbcp_Cfg.Options & bbcp_COMPRESS 
   && !(cxp = setup_CX(1, inFile->ioFD()))) return -ECANCELED;

// Start a thread for each data link we have
//
   for (i = 0; i < dlcount; i++)
       {if ((retc = bbcp_Thread_Start(bbcp_Buff2Net, "bbcp_Buff2Net",
                                (void *)data_link[i], &tid))<0)
           {bbcp_Emsg("SendFile",retc,"starting net thread for",fp->pathname);
            _exit(100);
           }
        link_tid[i] = tid;
        if (i >= iocount) {DEBUG("Thread " <<tid <<" assigned to data clocker");}
           else {DEBUG("Thread " <<tid <<" assigned to stream " <<i);}
       }

// Start the parent monitor
//
   Parent_Monitor.Start();

// Start the Transfer Time Limit
//
   if (bbcp_Cfg.TimeLimit)
      {TLimit = new bbcp_ProcMon();
       TLimit->Start(bbcp_Cfg.TimeLimit, &bbcp_BPool);
      }

// Read the whole file
//
   if (bbcp_Cfg.Options & bbcp_COMPRESS) retc=inFile->Read_All(bbcp_APool,1);
      else retc = inFile->Read_All(bbcp_BPool, bbcp_Cfg.Bfact);
   DEBUG("File read ended; rc=" <<retc);

// Wait for compression thread to end
//
   if (bbcp_Cfg.Options & bbcp_COMPRESS)
      {if ((tretc = (long)bbcp_Thread_Wait(cxp->TID))) retc = 128;
       DEBUG("File compression ended; rc=" <<tretc);
       delete cxp;
      }

// Make sure each link thread has terminated normally.
//
   for (i = 0; i < iocount; i++)
       {if ((tretc = (long)bbcp_Thread_Wait(link_tid[i]))) retc = 128;
        DEBUG("Thread " <<link_tid[i] <<" stream " <<i <<" ended; rc=" <<tretc);
       }

// All done
//
   if (TLimit) delete TLimit;
   Parent_Monitor.Stop();
   delete inFile;
   close(1); close(2);
   DEBUG("Process " <<getpid() <<" exiting with rc=" <<retc);
   exit(retc);
   return(retc);  // some compilers insist on a return in int functions
}

/******************************************************************************/
/*                       P r i v a t e   M e t h o d s                        */
/******************************************************************************/
/******************************************************************************/
/*                                c h k W s z                                 */
/******************************************************************************/
  
void bbcp_Node::chkWsz(int fd, int Final)
{
   int wbsz = bbcp_Net.getWBSize(fd, bbcp_Cfg.Options & bbcp_SRC);
   const char *fmode = (bbcp_Cfg.Options & bbcp_SRC ? "send"   : "recv");
   const char *smode = (bbcp_Cfg.Options & bbcp_SRC ? "Source" : "Target");
   const char *Wtype;
   char mbuff[256];
   int n;

// Figure out window type
//
   if (Final) Wtype = "a final";
      else    Wtype = (bbcp_Net.AutoTune() ? "initial" : "a fixed");

// Issue message
//
   n = sprintf(mbuff, "%s %s using %s %s window of %d\n",
                      smode, bbcp_Cfg.MyHost, Wtype, fmode, wbsz);
   write(STDERR_FILENO, mbuff, n);
}

/******************************************************************************/
/*                             I n c o m m i n g                              */
/******************************************************************************/
  
int bbcp_Node::Incomming(bbcp_Protocol *protocol)
{
    int i, retc, minport, maxport;
    bbcp_Link *link;

// Find the port number we should use for incomming connections
//
   bbcp_Net.findPort(minport, maxport);

// Set up the default ports first if we didn't find specified ones
//
   if (minport || maxport
   || ((retc = bbcp_Net.Bind(BBCP_DFLTMINPORT, BBCP_DFLTMAXPORT, 1, -1)) < 0))
   if ((retc = bbcp_Net.Bind(minport, maxport,
               bbcp_Cfg.bindtries, bbcp_Cfg.bindwait)) < 0)
      return retc;

// Report the port number we have chosen
//
   protocol->setCBPort(retc);

// Establish all of the connections
//
   while(dlcount < bbcp_Cfg.Streams)
       {if (!(link = bbcp_Net.Accept())) break;
        if (!(retc = protocol->Login(link, 1))) 
           {link->LinkNum = dlcount; data_link[dlcount++] = link;}
           else if (retc < 0) {delete link; break;}
       }

// Unbind the network and make sure we have all of the agreed upon links
//
   bbcp_Net.unBind();
   if (dlcount < bbcp_Cfg.Streams) return Recover("Accept");
   iocount = dlcount;

// Initialize the buddy pipeline; a patented way of ensuring maximum parallelism
//
   if (dlcount > 1 && (bbcp_Cfg.Options & (bbcp_SRC|bbcp_ORDER)))
      {i = dlcount-1;
       data_link[i]->setBuddy(data_link[0]);
       while(i--) data_link[i]->setBuddy(data_link[i+1]);
       bbcp_Link::setNudge();
      }

// Determine what the actual window size is (only if verbose)
//
   if (bbcp_Cfg.Options & bbcp_BLAB) chkWsz(data_link[0]->FD());
   return 0;
}
  
/******************************************************************************/
/*                              O u t g o i n g                               */
/******************************************************************************/
  
int bbcp_Node::Outgoing(bbcp_Protocol *protocol)
{
   int i, retc;
   bbcp_Link *link;

// Establish the control connection first
//
   if ((link = bbcp_Net.Connect(bbcp_Cfg.CBhost, bbcp_Cfg.CBport, 3)))
      if ((retc = protocol->Login(link, 0)) < 0)
         {delete link; link = 0;}

// Now establish all data channels if we have a control channel
//
   if (link)
      {pthread_t tid, T_id[BBCP_MAXSTREAMS];

       // Start threads for data connections
       //
       for (i = 0; i < bbcp_Cfg.Streams; i++)
           {if ((retc=bbcp_Thread_Start(bbcp_Connect,"bbcp_Connect",
                                        (void *)protocol,&tid))<0)
               {bbcp_Emsg("Outgoing", retc, "starting connect thread");
                _exit(100);
               }
            T_id[i] = tid;
#ifdef CRAY_X1E
            usleep(1);
#endif
           }
       for (i = 0; i < bbcp_Cfg.Streams; i++)
           {if (!(link = (bbcp_Link *)bbcp_Thread_Wait(T_id[i]))) break;
            link->LinkNum = dlcount; data_link[dlcount++] = link;
           }
       }
   iocount = dlcount;

// Make sure we have all of the required links
//
   if (dlcount < bbcp_Cfg.Streams) return Recover("Connect");

// Determine what the actual window size is (only if verbose)
//
   if (bbcp_Cfg.Options & bbcp_BLAB) chkWsz(data_link[0]->FD());

// Initialize the buddy pipeline; a patented way of ensuring maximum parallelism
//
   if (dlcount > 1 && (bbcp_Cfg.Options & (bbcp_SRC|bbcp_ORDER)))
      {i = dlcount-1;
       data_link[i]->setBuddy(data_link[0]);
       while(i--) data_link[i]->setBuddy(data_link[i+1]);
       bbcp_Link::setNudge();
      }
   return 0;
}
  
/******************************************************************************/
/*                               R e c o v e r                                */
/******************************************************************************/

int bbcp_Node::Recover(const char *who)
{
    char mbuff[256];
    sprintf(mbuff, "%d of %d data streams.", dlcount, bbcp_Cfg.Streams);
    bbcp_Fmsg(who, "Unable to allocate more than", mbuff);
    while(dlcount) data_link[--dlcount]->Close();
    return -1;
}
 
/******************************************************************************/
/*                                R e p o r t                                 */
/******************************************************************************/
  
void bbcp_Node::Report(double ttime, bbcp_FileSpec *fp, bbcp_File *ioFile,
                       bbcp_ZCX *cxp)
{
float  cratio;
double xtime, xrate;
long long xbytes, cxbytes;
const char *xType;
char buff[128], Line[2048];
int n;

// Calculate compression ratio
//
   xbytes = ioFile->Stats(xtime);
   if (cxp)
      {if (!(cxbytes = cxp->Bytes())) cratio = 0.0;
          else cratio = ((float)(xbytes*10/cxbytes))/10.0;
       sprintf(buff, " compressed %.1f", cratio);
      } else *buff = 0;

// Print the summary
//
   xrate = ((double)xbytes)/ttime*1000.0; xType = bbcp_Config::Scale(xrate);
   n = sprintf(Line, "File %s created; %lld bytes at %.1f %sB/s%s\n",
               fp->targpath, xbytes, xrate, xType, buff);
   write(STDERR_FILENO, Line, n);
   if (!(bbcp_Cfg.Options & bbcp_BLAB)) return;

// Tell user how many reorder events there were
//
   n = sprintf(Line, "%d buffers used with %d reorders; peaking at %d.\n",
              bbcp_BPool.BuffCount(), ioFile->bufreorders, ioFile->maxreorders);
   write(STDERR_FILENO, Line, n);
}
 
/******************************************************************************/
/*                     s e t u p _ C o m p r e s s i o n                      */
/******************************************************************************/
  
bbcp_ZCX *bbcp_Node::setup_CX(int deflating, int iofd)
{
   int retc, clvl;
   bbcp_BuffPool *ibp, *rbp, *obp;
   bbcp_ZCX *cxp;
   pthread_t tid;

// Initialize the processing parameters
//
   if (deflating)
      {ibp = &bbcp_APool;
       rbp = &bbcp_APool;
       obp = &bbcp_BPool;
       clvl = (bbcp_Cfg.Complvl ? bbcp_Cfg.Complvl : 1);
      } else {
       ibp = &bbcp_CPool;
       rbp = &bbcp_BPool;
       obp = &bbcp_APool;
       clvl = 0;
      }

// Allocate buffers in the A pool
//
   if (bbcp_APool.Allocate(bbcp_Cfg.BNum, bbcp_Cfg.RWBsz, !deflating))
      return 0;

// Allocate a new compression/expansion object
//
   cxp = new bbcp_ZCX(ibp, rbp, obp, clvl, iofd,
             (int)(bbcp_Cfg.Options & (clvl ? bbcp_LOGCMP : bbcp_LOGEXP)));

// Start the compression/expansion thread
//
   if ((retc = bbcp_Thread_Start(bbcp_doCX, "bbcp_doCX", (void *)cxp, &tid))<0)
      {bbcp_Emsg("File", retc, "starting", 
                 (char *)(deflating ? "compression" : "expansion"),
                 (char *)" thread.");
       _exit(100);
      }
   cxp->TID = tid;
   DEBUG("Thread " <<tid <<" assigned to cx stage.");

// Return the compression object
//
   return cxp;
}
 
/******************************************************************************/
/*                                 U s a g e                                  */
/******************************************************************************/
  
int bbcp_Node::Usage(const char *who, char *buff, int blen)
{
      int   Tsec, Ssec, Usec;

      Tsec = bbcp_OS.Usage(Ssec, Usec);

      return snprintf(buff, blen, "%s cpu=%.3f (sys=%.3f usr=%.3f).\n", who,
             ((float)Tsec)/1000.0, ((float)Ssec)/1000.0, ((float)Usec)/1000.0);
}
