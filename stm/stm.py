#!/usr/bin/python
#
#
# lSTM - Linux ServeToMe
#
# Kate Wilkins (c)2009
# http://code.google.com/p/servetome/
#
# Version History
# ---------------
#
# Date      Who  Version Notes
# ------------------------------------
# 02/06/10  KW   v100    Initial release
# 18/06/10  KW   v300    Modified for ServeToMe 3.x operation
# 01/07/10  KW   v301    Started coding http digest authentication, bugfix to subtitle params on ffmpeg-stm launch
# 01/07/10  KW   v302    Reworked directory sorting code, added sort descending & date
# 01/07/10  CT   v303    JSON UTF-8 Fix
# 06/07/10  KW   v304    Reworking of doStream to stop re-tcoding already done segs
# 13/07/10  KW   v305    New streaming implementation complete, still some bugs
# 14/07/10  KW   v306    Finished debugging new streaming, added proper rate option usage to doStream
# 14/07/10  KW   v307    Fixed error on index.m3u8 generation, now generated everytime requested
# 16/07/10  CT   v308    Flat file modes added, aud/subs options fix
# 14/08/10  KW   v309    Updates for client v3.1.1 changes that caused stm to break, "id" & "rate"
# 16/09/10  KW   v310    Fix for 64bit machines, transcodeMetadata unpack "l" to "=l", line 420
#
#
# TODO
# ----
# HTTP digest implementation

STM_VERSION = "3.10"

import string,cgi,time
import ConfigParser
import sys
import json
import os
import struct
import subprocess
import urllib
import random
import traceback
import Cookie
import shutil
import time
import signal
from threading import Event, Thread
from urlparse import urlparse
from BaseHTTPServer import BaseHTTPRequestHandler, HTTPServer
from operator import itemgetter

listenPort=9969
username=""
password=""
tempDir=""
execDir=""
directoryList=""
extensionList=("aif","m2ts","ts","flac","wmv","ogm","ogg","wma","m4a","vob","dif","dv","flv","asf","mp2","mp3","ac3","aac","mpeg4","mp4","m4v","mpeg","mkv","mpg","mov","gvi","avi")
debugEnable=False
sessions={}
commandHandle=""
SEG_LOOK_AHEAD=5
DEFAULT_SEGLEN=4
SESSION_TIMEOUT=1200
SESSION_TIMEOUT_STEP=10
#SESSION_TIMEOUT=10
#SESSION_TIMEOUT_STEP=1
COOKIENAME="STREAMSESSIONID"
debugHandle=None
commandHandle=None

ratelookupBPS={'veryhigh':2048000,'high':1440000,'midhigh':720000,'mid':360000,'midlow':144000,'low':96000,'verylow':64000,
'wifiveryhigh':2048000,'wifihigh':1440000,'wifimidhigh':720000,'wifimid':360000,'wifimidlow':144000,'wifilow':96000,'wifiverylow':64000,
'localveryhigh':2048000,'localhigh':1440000,'localmidhigh':720000,'localmid':360000,'localmidlow':144000,'locallow':96000,'localverylow':64000,
'3gveryhigh':720000,'3ghigh':720000,'3gmidhigh':720000,'3gmid':360000,'3gmidlow':144000,'3glow':96000,'3gverylow':64000}

ratelookupFFM={'veryhigh':'veryhigh','high':'high','midhigh':'midhigh','mid':'mid','midlow':'midlow','low':'low','verylow':'verylow',
'wifiveryhigh':'veryhigh','wifihigh':'high','wifimidhigh':'midhigh','wifimid':'mid','wifimidlow':'midlow','wifilow':'low','wifiverylow':'verylow',
'localveryhigh':'veryhigh','localhigh':'high','localmidhigh':'midhigh','localmid':'mid','localmidlow':'midlow','locallow':'low','localverylow':'verylow',
'3gveryhigh':'veryhigh','3ghigh':'high','3gmidhigh':'midhigh','3gmid':'mid','3gmidlow':'midlow','3glow':'low','3gverylow':'verylow'}


# RepeatTimer class - Copyright (c) 2009 Geoffrey Foster
# http://g-off.net/software/a-python-repeatable-threadingtimer-class
class RepeatTimer(Thread):
    def __init__(self, interval, function, iterations=0, args=[], kwargs={}):
        Thread.__init__(self)
        self.interval = interval
        self.function = function
        self.iterations = iterations
        self.args = args
        self.kwargs = kwargs
        self.finished = Event()
 
    def run(self):
        count = 0
        while not self.finished.is_set() and (self.iterations <= 0 or count < self.iterations):
            self.finished.wait(self.interval)
            if not self.finished.is_set():
                self.function(*self.args, **self.kwargs)
                count += 1
 
    def cancel(self):
        self.finished.set()

def debugLog(message):
    global debugHandle
    if debugEnable: 
        print "{0} {1}".format(timestamp(),message)
        if not debugHandle is None: debugHandle.write("{0} {1}\n".format(timestamp(),message))

def timestamp():
    lt = time.localtime(time.time())
    return "%02d/%02d/%02d %02d:%02d:%02d" % (lt[2], lt[1], lt[0]-2000, lt[3], lt[4], lt[5])
 
def genHash2(length=8, chars=string.letters + string.digits):
    return ''.join([random.choice(chars) for i in range(length)])

def genHash():
    #return genHash2(8)+"-"+genHash2(4)+"-"+genHash2(4)+"-"+genHash2(4)+"-"+genHash2(12)
    return genHash2(12)

def makeM3U8(path,name,seglen,duration):
    segcount=int(duration/seglen)+1
    f=open(path+name+".m3u8","w")
    f.write("#EXTM3U\n")
    f.write("#EXT-X-TARGETDURATION:4\n")
    segno=0
    while(segno<segcount):
        f.write("#EXTINF:4,\n")
        f.write("{0}-{1:05d}.ts\n".format(name,segno))
        segno+=1
    f.write("#EXT-X-ENDLIST\n")
    f.close()
    
def isDirectory(pathbits):
    fpath=buildPath(pathbits)
    if os.path.isdir(fpath)==True:
        return True
    else:
        return False

def buildPath(pathbits):
    if len(pathbits)==0 or pathbits[0]=="":
        fpath="."
    else:
        fpath=directoryList[int(pathbits[0])]
        loop=1
        while loop<len(pathbits):
            if loop>0: fpath = fpath + "/"
            fpath = fpath + pathbits[loop]
            loop=loop+1
    return fpath

    
def sessionNew(client=""):
    if client=="": client=genHash()
    sessions[client]={}
    sessions[client]['hash']=client
    sessions[client]['directory']=tempDir+"/session."+sessions[client]['hash'][0:12]
    sessions[client]['idle']=0
    sessions[client]['authenticated']=False
    # Default params
    if sys.platform=='win32': sessions[client]['directory']=sessions[client]['directory'].replace('/','\\')
    # Does this dir already exist
    if not os.path.exists(sessions[client]['directory']): os.makedirs(sessions[client]['directory'])
    debugLog("sessionNew(): Created new session for client:{0}".format(client))
    debugLog("sessionNew(): SessionDir:{0}".format(sessions[client]['directory']))
    return client

def sessionKill(client):
    # Kill any open session processes
    transcoderKill(client)
    # Remove temporary directorys
    debugLog('sessionKill(): Unlinking - {0}'.format(sessions[client]['directory']))
    shutil.rmtree(sessions[client]['directory'],ignore_errors=True)
    del sessions[client]

def sessionIdle():
    #debugLog('sessionIdle() - Tick')
    for client in sessions:
        if sessions[client]['idle']>SESSION_TIMEOUT:
            debugLog('sessionIdle() - {0} timed out'.format(client))
            sessionKill(client)
            # We can only kill one per call, it breaks the for iterator
            # when we delete its own entries
            break
        else:
            sessions[client]['idle']+=SESSION_TIMEOUT_STEP

            
            
def transcoderNew(client,options="",fpath="",movieDir="",rate="norate",segstart=0):
    transcoderKill(client)
    if segstart<0: segstart=0
    sessions[client]['transcoder']={}
    sessions[client]['transcoder']['device']='iPad'
    sessions[client]['transcoder']['transport']='wifi'
    sessions[client]['transcoder']['rate']=rate
    sessions[client]['transcoder']['segstart']=segstart
    sessions[client]['transcoder']['seglen']=DEFAULT_SEGLEN
    sessions[client]['transcoder']['seglast']=0
    sessions[client]['transcoder']['segack']=0
    sessions[client]['transcoder']['segcount']=int(sessions[client][fpath]['duration']/DEFAULT_SEGLEN)+1
    sessions[client]['transcoder']['audio']='any'
    sessions[client]['transcoder']['subs']='nil'
    sessions[client]['transcoder']['gain']='0.0'
    sessions[client]['transcoder']['art']='yes'
    sessions[client]['transcoder']['srtdir']='default'
    sessions[client]['transcoder']['srtenc']='UTF-8'
    sessions[client]['transcoder']['duration']=sessions[client][fpath]['duration']
    sessions[client]['transcoder']['source']=fpath
    sessions[client]['transcoder']['dest']=movieDir

    # Extract params from stream command and overide any defaults
    options=options.split(',')
    for opt in options:
        optname=""
        optval=""
        if(opt): optname,optval=opt.split('=')
        if optname=='rate' and optval!="":
            sessions[client]['transcoder']['transport']=optval.lower()
        elif optname=='device' and optval!="":
            sessions[client]['transcoder']['device']=optval.lower()
        elif (optname=='audio' or optname=='aud') and optval!="":
            sessions[client]['transcoder']['audio']=optval
        elif (optname=='subtitles' or optname=='subtitle' or optname=='sub') and optval!="":
            sessions[client]['transcoder']['subs']=optval
        elif optname=='gain' and optval!="":
            sessions[client]['transcoder']['gain']=optval
        elif optname=='art' and optval!="":
            sessions[client]['transcoder']['art']=optval
        elif optname=='srtDirection' and optval!="":
            sessions[client]['transcoder']['srtdir']=optval
        elif optname=='srtEncoding' and optval!="":
            sessions[client]['transcoder']['srtenc']=optval

    # Perform rate conversion as it could be a mix of transport+rate
    if ratelookupFFM.has_key(sessions[client]['transcoder']['rate']):
        sessions[client]['transcoder']['rate']=ratelookupFFM[sessions[client]['transcoder']['rate']]

    # Before we finish we need to check the start segment against ones we already have
    # in stock, if we've already got it then dont make it again, push the startseg ahead
    rate=sessions[client]['transcoder']['rate']
    segstart=sessions[client]['transcoder']['segstart']
    
    if fpath!="" and sessions[client].has_key(fpath) and sessions[client][fpath].has_key(rate):
        while segstart<len(sessions[client][fpath][rate]) and sessions[client][fpath][rate][segstart]=="1":
            segstart+=1
        debugLog("transcoderNew() - bumped segstart to {0}".format(segstart))
        # Save the new start segment
        sessions[client]['transcoder']['segstart']=segstart
    
    

def transcoderKill(client):
    if sessions[client].has_key('transcoder') and sessions[client]['transcoder'].has_key('process'):
        ffmpeg=sessions[client]['transcoder']['process']
        ffmpeg.poll()
        if ffmpeg.returncode==None:
            ffmpeg.stdin.write("\ndiediedie\n")
            #ffmpeg.wait()
            time.sleep(0.1)
            ffmpeg.poll()
            if ffmpeg.returncode==None:
                ffmpeg.kill()
        del sessions[client]['transcoder']['process']
        sessions[client]['transcoder']['seglist'].close()
        debugLog("transcoderKill(): Killed")
    #else:
    #    debugLog("transcoderKill(): Nothing to kill")

def transcoderLaunch(client):

    if not sessions[client].has_key('transcoder'):
        debugLog("transcoderLaunch(): Missing client transcoder key???")
    elif sessions[client]['transcoder']['segstart']>=sessions[client]['transcoder']['segcount']:
        debugLog("transcoderLaunch(): Nothing to transcode segstart>=segcount")        
    else:
        transcoder=sessions[client]['transcoder']
        
        # Abort TRANSCODE and restart if already running, we only allow ONE transcoder per client
        transcoderKill(client)

        # Temp file to replace stdout!
        segFile=transcoder['dest']+"seglist.out"
                
        # Now start the stream compilation in the background
        execStr=[execDir+'/ffmpeg-stm','transcode',transcoder['source'],transcoder['dest']+transcoder['rate'],transcoder['rate'],str(transcoder['seglen']),str(transcoder['segstart']),transcoder['audio'],transcoder['subs'],transcoder['gain'],transcoder['art'],transcoder['srtenc'],transcoder['srtdir']]
        if sys.platform=='win32':
            execStr[0]=execDir+'/ffmpeg-stm.exe'
            execStr=subprocess.list2cmdline(execStr)
            execStr=execStr.replace('/','\\')
            segFile=segFile.replace('/','\\')
        elif sys.platform=='darwin':
            execStr[0]=execDir+'/ffmpeg-stm_osx'

        debugLog("transcoderLaunch(): Exec: {0}".format(execStr))

        foutput = open(segFile,'wt')
        finput = open(segFile,'rt')
        # Spawn a sub process to make the thumbnail, capture output to tempfile, wait for it to complete
        ffmpeg = subprocess.Popen(execStr, stdin=subprocess.PIPE, stdout=foutput, stderr=subprocess.PIPE)

        # Buffer up 10 segments
        ackack="ack"*SEG_LOOK_AHEAD
        ffmpeg.stdin.write(ackack)

        # Save the open process parameters in the session dictionary
        transcoder['process']=ffmpeg
        transcoder['seglist']=finput

        # Wait for duration spit out + 1st segment output, stops race on 1st seg read
        while 1:
            transcoder['process'].poll()
            if transcoder['process'].returncode!=None: break
            status=transcoder['seglist'].readline()
            # Extract segment number from output
            if not status=="":
                debugLog("transcoderLaunch(): Got '{0}'".format(status))
                junk,duration=status.rsplit(" ",1)
                # Register the duration from the transcode with the ufid, we'll want it later if there is a quick restart
                transcoder['duration']=float(duration)
                sessions[client][transcoder['source']]['duration']=float(duration)
                break
            else:
                # Spin for a short time
                #debugLog("transcoderLaunch(): SPINNING")
                time.sleep(0.05)

        # Initialise the dictionary for segment loading 
        transcoder['seglast']=transcoder['segstart']-1

def transcoderProcess(client):
    if not sessions[client].has_key('transcoder'):
        debugLog("transcoderProcess(): Missing client transcoder key???")
        return False
    elif not sessions[client]['transcoder'].has_key('process'):
        debugLog("transcoderProcess(): No active transcoder")
        return False
    else:
        transcoder=sessions[client]['transcoder']

        # If we have an active transcoder then process any pending seg completes
        # and mark in the mapping tables as being completed
        transcoder['process'].poll()
        if transcoder['process'].returncode==None:
            #debugLog("transcoderProcess(): Transcoder is running")
            pass
        else:
            debugLog("transcoderProcess(): Transcoder has finished, cleaning up")
            del transcoder['process']
            
        #debugLog("transcoderProcess(): Filemap IN ={0}".format(sessions[client][transcoder['source']][transcoder['rate']][:20]))
        count=0
        while 1:
            status=transcoder['seglist'].readline()
            # Extract segment number from output
            if status!="":
                debugLog("transcoderProcess(): Got '{0}'".format(status[:-1]))
                junk,status=status.rsplit(" ",1)
                # Mark as received in the file map array
                idx=int(status)
                fmap=sessions[client][transcoder['source']][transcoder['rate']]
                fmap = fmap[0:idx] + "1" + fmap[idx+1:]
                sessions[client][transcoder['source']][transcoder['rate']]=fmap
                # We need to keep account of the last segment acked, allows us to wait
                # for next seg in the doStream
                transcoder['seglast']=idx
                # Increase count for later ACK
                count+=1
            else:
                #debugLog("transcoderProcess(): Transcoder has nothing for us")
                break
        if count>0: debugLog("transcoderProcess(): Filemap OUT={0}".format(sessions[client][transcoder['source']][transcoder['rate']][:20]))

        # Ack same number of segs as we've received, always keep the tcoder
        # SEG_LOOK_AHEAD segments ahead of the game
        if transcoder.has_key('process'):
            transcoder['process'].poll()
            pstat=transcoder['process'].returncode
            if pstat==None:
                if count>0:
                    # Send the ack, there is a race here, it might finish between the poll & the ack!
                    ackstr="ack"*count
                    transcoder['process'].stdin.write(ackstr)
                    debugLog("transcoderProcess(): Ack - More segs({0}) please".format(str(count)))
                #else:
                    #debugLog("transcoderProcess(): Nothing to ACK")
            else:
                debugLog("transcoderProcess(): Ack - Process is complete or dead")

        return True

def transcoderMetadata(client,fpath):
    try:
        response=""
        tempFile=tempDir+'/thumbnail.bin'
        if sys.platform=='win32':
            execStr='"'+execDir+'/ffmpeg-stm.exe" metadata "'+fpath+'"'
            execStr=execStr.replace('/','\\')
            tempFile=tempFile.replace('/','\\')
        elif sys.platform=='darwin':
            execStr=[execDir+'/ffmpeg-stm_osx','metadata',fpath]
        else:
            execStr=[execDir+'/ffmpeg-stm','metadata',fpath]

        debugLog("transcoderMetadata(): Exec: {0}".format(execStr))
        debugLog("transcoderMetadata(): Thum: {0}".format(tempFile))

        # Spawn a sub process to make the thumbnail, capture output to tempfile, wait for it to complete
        foutput = open(tempFile,'w')
        ffmpeg = subprocess.Popen(execStr, stdout=foutput,stderr=subprocess.PIPE)
        ffmpeg.wait()
        foutput.close()
        
        # Now open the result of the exec
        if ffmpeg.returncode==0 or 1:
            stream=open(tempFile,'rb')
            # Read length of JSON segment + JSON + whole JPEG segment
            json_length,=struct.unpack('=l', stream.read(4))
            response=stream.read()
            stream.close()

            decode=json.loads(response[0:json_length])
            duration=int(decode['length'])
            if not sessions[client].has_key(fpath): sessions[client][fpath]={}
            sessions[client][fpath]['duration']=float(duration)
        else:
            debugLog("transcoderMetadata(): Invalid return code from ffmpeg???")
            response=0,""
    except os.error:
        debugLog("transcoderMetadata(): OS Error thrown???")
        response=0,""
    return json_length,response




def doRoot(self,url,options):
    response="<html>"
    response+="<head><title>ServeToMe</title></head>"
    response+="<body><large>ServeToMe("+STM_VERSION+") is running on port "+str(listenPort)+"</large></body>"
    response+="</html>"
    
    debugLog("doRoot(): Response: {0}".format(response))

    self.send_response(200)
    self.send_header('Servetome-Version',STM_VERSION)
    self.send_header('Connection','close')
    self.send_header('Content-Length',len(response))
    self.send_header('Content-Type','text/html')
    self.end_headers()
    self.wfile.write(response)
    
def doStream(client,self,url,options):
    # Reset session idle timer
    sessions[client]['idle']=0

    # Extract options from the PATH!
    if len(url)>1:
        options=url[0]
        del url[0]
        
    # Extract the file request from the path
    if len(url)>1:
        request=url[len(url)-1]
        del url[len(url)-1]
    
    debugLog("doStream(): Options: {0}".format(options))
    debugLog("doStream(): Request: {0}".format(request))
        
    debugLog("doStream(): Path: {0}".format(url))
    fpath=buildPath(url)
    debugLog("doStream(): Path: {0}".format(fpath))

    # Check if a key exists for this file for this client, the key is
    # critical for us as it contains all of the specific movie info
    # that we need for transoding 
    if sessions[client].has_key(fpath) and sessions[client][fpath].has_key("ufid") :
        movieDir=sessions[client]['directory']+"/"+sessions[client][fpath]['ufid']+"/"
    else:
        if not sessions[client].has_key(fpath): sessions[client][fpath]={}
        debugLog("doStream(): Creating session data for this movie: {0}".format(fpath))

        # Hash the film name to UFID to save a long path being appended fpath=UFID for the transcoder output
        sessions[client][fpath]['ufid']=genHash()
        
        movieDir=sessions[client]['directory']+"/"+sessions[client][fpath]['ufid']+"/"
        if sys.platform=='win32': movieDir=movieDir.replace('/','\\')
        debugLog("doStream(): Session Dir for this movie: {0}".format(movieDir))

        # Does this dir already exist
        if os.path.exists(movieDir):
            debugLog("doStream(): UUID directory exists, it should not exist at this point!!")
        else:
            os.makedirs(movieDir)

        # Check we have a duration for this movie, there is the possibility for STM
        # to start stright into a movie without ever having gone through the metadata
        # call, so we may have to call metadata ourselves to get the duration before
        # going any further!!
        if not sessions[client][fpath].has_key('duration'):
            debugLog("doStream(): Streaming has started without metadata request, missing duration")
            transcoderMetadata(client,fpath)
        
        # Build a new transcoder dictionary
        transcoderNew(client,options,fpath,movieDir)
                
        # Generate all of the UUID/<rate>.m3u8, index.m3u8 will say whays actually available
        makeM3U8(movieDir,"veryhigh",DEFAULT_SEGLEN,sessions[client][fpath]['duration'])
        makeM3U8(movieDir,"high",DEFAULT_SEGLEN,sessions[client][fpath]['duration'])
        makeM3U8(movieDir,"midhigh",DEFAULT_SEGLEN,sessions[client][fpath]['duration'])
        makeM3U8(movieDir,"mid",DEFAULT_SEGLEN,sessions[client][fpath]['duration'])
        makeM3U8(movieDir,"midlow",DEFAULT_SEGLEN,sessions[client][fpath]['duration'])
        makeM3U8(movieDir,"low",DEFAULT_SEGLEN,sessions[client][fpath]['duration'])
        makeM3U8(movieDir,"verylow",DEFAULT_SEGLEN,sessions[client][fpath]['duration'])

        # Create the file maps
        duration=int(sessions[client][fpath]['duration'])
        if duration==0: duration=(5*60*60)/DEFAULT_SEGLEN
        for rate in ratelookupBPS:
            sessions[client][fpath][rate]="0"*((duration/DEFAULT_SEGLEN)+1)
            
    # Handle any completed segments
    transcoderProcess(client)

    # Check if m3u8 is enquiring on a different stream, we may need to restart
    if request.endswith("m3u8")==True:
        # Extract the rate parameter from the filename
        rate,junk1,junk2=request.partition(".")
        
        if rate=="index":
            # Build a new transcoder dictionary
            transcoderNew(client,options,fpath,movieDir)
            debugLog("doStream(): Building index.m3u8")
            # We must now build the index files for our UFID
            try:
                device=sessions[client]['transcoder']['device']
                transport=sessions[client]['transcoder']['transport']
                # Generate UUID/index.m3u8
                f=open(movieDir+"index.m3u8","w")
                f.write("#EXTM3U\n")
                if ratelookupBPS.has_key(transport):
                    f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH={0}\n".format(ratelookupBPS[transport]))
                    f.write("{0}.m3u8\n".format(ratelookupFFM[transport]))                
                elif transport=='local':
                    if device=='ipad' or device=='iphone4':
                        f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=2048000\n")
                        f.write("veryhigh.m3u8\n")
                        f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=1440000\n")
                        f.write("high.m3u8\n")
                    else:
                        f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=1440000\n")
                        f.write("high.m3u8\n")
                        f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=2048000\n")
                        f.write("veryhigh.m3u8\n")
                    f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=720000\n")
                    f.write("midhigh.m3u8\n")
                elif transport=='wifi':
                    if device=='ipad' or device=='iphone4':
                        f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=2048000\n")
                        f.write("veryhigh.m3u8\n")
                        f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=1440000\n")
                        f.write("high.m3u8\n")
                    else:
                        f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=1440000\n")
                        f.write("high.m3u8\n")
                        f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=2048000\n")
                        f.write("veryhigh.m3u8\n")
                    f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=720000\n")
                    f.write("midhigh.m3u8\n")
                    f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=360000\n")
                    f.write("mid.m3u8\n")
                    f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=144000\n")
                    f.write("midlow.m3u8\n")
                elif transport=='3g':
                    f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=144000\n")
                    f.write("midlow.m3u8\n")
                    f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=1440000\n")
                    f.write("high.m3u8\n")
                    f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=720000\n")
                    f.write("midhigh.m3u8\n")
                    f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=360000\n")
                    f.write("mid.m3u8\n")
                    f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=96000\n")
                    f.write("low.m3u8\n")
                    f.write("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=64000\n")
                    f.write("verylow.m3u8\n")
                else:
                    debugLog("doStream(): ERROR invalid transport in request")
                f.close()
            
            except:
                debugLog("doStream(): We had an exception during the file build")
                debugLog("doStream(): {0}".format(traceback.format_exc()))

        elif rate!=sessions[client]['transcoder']['rate']:
            # Register the transcoder options & kick off the codec, lets hope we mapped the right rate!
            # Guess the seg as seglast-1
            segstart=sessions[client]['transcoder']['seglast']-1
            transcoderNew(client,options,fpath,movieDir,rate,segstart)
            transcoderLaunch(client)
        
    if request.endswith("ts")==True:
        # Check if this segment has not been completed
        segwanted=int(request[-8:-3])
        rate,junk1,junk2=request.partition("-")
        
        if sessions[client][fpath][rate][segwanted]=="0":
            # Check if this segment is about to come from the transcoder, if so we may wait
            # also have any transcoder params changed if so we must restart THEN wait
            if segwanted>sessions[client]['transcoder']['seglast']+1 or fpath!=sessions[client]['transcoder']['source'] or rate!=sessions[client]['transcoder']['rate'] or segwanted<sessions[client]['transcoder']['segstart']:
                debugLog("doStream(): Forcing transcoder restart, new file({0}), rate({1}), segment({2})".format(fpath,rate,segwanted))
                # Build a new transcoder dictionary
                transcoderNew(client,options,fpath,movieDir,rate,segwanted)
                transcoderLaunch(client)

            # Wait for our segment here
            while transcoderProcess(client):
                if sessions[client][fpath][rate][segwanted]=="1":
                    debugLog("doStream(): Segwait - Got {0}".format(request))
                    break
                else:
                    # Spin for a short time
                    #debugLog("doStream(): SPINNING")
                    time.sleep(0.25)
        else:
            debugLog("doStream(): Already have {0} in the filemap".format(request))

    # Path to output file
    fpath=movieDir+request
    if sys.platform=='win32': fpath=fpath.replace('/','\\')

    debugLog("doStream(): Serving: {0}".format(fpath))
    try:
        stream=open(fpath,'rb')
        response=stream.read()
        stream.close()
        chocchip=Cookie.SimpleCookie()
        chocchip[COOKIENAME]=sessions[client]['hash']
        self.send_response(200)
        self.send_header('Set-Cookie',chocchip.output(header=''))
        self.send_header('Servetome-Version',STM_VERSION)
        self.send_header('Connection','close')
        self.send_header('Content-Length',len(response))
        self.send_header('Content-Type','application/vnd.apple.mpegurl')
        self.end_headers()
        self.wfile.write(response)
    except:
        debugLog("doStream(): Exception raised, sending 404")
        self.send_error(404,'Segment Not Found: %s' % self.path)

def doMetadata(client,self,url,options):
    # Reset session idle timer
    sessions[client]['idle']=0

    debugLog("doMetadata(): Path: {0}".format(url))
    fpath=buildPath(url)
    debugLog("doMetadata(): Path: {0}".format(fpath))

    json_length,response=transcoderMetadata(client,fpath)
    if response!="" and json_length!=0:
        # Send headers
        chocchip=Cookie.SimpleCookie()
        chocchip[COOKIENAME]=sessions[client]['hash']
        self.send_response(200)
        self.send_header('Set-Cookie',chocchip.output(header=''))
        self.send_header('Servetome-Version',STM_VERSION)
        self.send_header('Connection','close')
        self.send_header('Json-Length',json_length)
        self.send_header('Content-Length',len(response))
        self.send_header('Content-Type','application/json')
        self.end_headers()
        self.wfile.write(response)
    else:
        self.send_error(500,'Decoding error on: %s' % self.path)
            
       
def doDirectory(client,self,url,options):
    # Reset session idle timer
    sessions[client]['idle']=0

    # Now sort the response table and output
    class Order:
        none=0
        ascending=1
        descending=2
        
    class Sort:
        ident=0            
        name=1
        type=2
        date=3
        none=4
        
    class Hierarchy:
        folder=0
        flat_folder=1
        flat=2
        
    order=Order.none
    sort=Sort.none
    hierarchy=Hierarchy.folder
    
    for opt in options:
        optname=""
        optval=""
        if(opt): optname,optval=opt.split('=')
        if optname=='order':
            if optval=='ascending':
                order=Order.ascending;
            elif optval=='descending':
                order=Order.descending;
            else:
                order=Order.ascending;
        if optname=='sort':
            if optval=='name':
                sort=Sort.name;
            elif optval=='type':
                sort=Sort.type;
            elif optval=='date':
                sort=Sort.date;
            else:
                sort=Sort.none;
        if optname=='hierarchy':
            debugLog("Option Value : " + optval)
            if optval=='folder':
                hierarchy=Hierarchy.folder;
            elif optval=='flattenAndSort':
                hierarchy=Hierarchy.flat;
            elif optval=='flatten':
                hierarchy=Hierarchy.flat_folder;
            else:
                hierarchy=Hierarchy.folder;

    try:
        response=""
        itemlist=[]
        rootlevel=False
        
        if len(url)==0:
            rootlevel=True
            debugLog("doDirectory(): Shares: {0}".format(url))
            # Enumerate the shares
            # json: name type
            loop=0
            while loop<len(directoryList):
                dirnam=directoryList[loop].split('/')[len(directoryList[loop].split('/'))-1]
                itemlist.append((str(loop),dirnam,'folder',0))
                loop=loop+1
        else:
            debugLog("doDirectory(): Path: {0}".format(url))
            fpath=buildPath(url)
            debugLog("doDirectory(): Path: {0}".format(fpath))
            # Enumerate share/path combo
            dirList=os.listdir(fpath)
            debugLog("doDirectory(): Found: {0}".format(dirList))
            for item in dirList:
                mtime=os.stat(fpath+"/"+item).st_mtime
                if os.path.isdir(fpath+"/"+item) == True:
                    if item[0]!=".":

                        # Define a new list to store a list of all filenames recursively in a directory
                        flatList=[]

                        # If this is a flat list + sort then keep folder ordering then sort the file list before returning
                        if hierarchy==Hierarchy.flat_folder:
                            flatList=listFiles(fpath, item)
                            flatList=sortList(flatList, sort, order)
                        # If this is a flat list, then don't sort now and wait for the final sort
                        elif hierarchy==Hierarchy.flat:
                            flatList=listFiles(fpath, item)
                        # If the user did not specify flat mode, then just add this directory
                        else:
                            itemlist.append(('',item,'folder',mtime))
                            
                        # If there is atleast something in the recursive list, we add it to the main list
                        if flatList>0:
                            itemlist.extend(flatList)

                else:
                    lowitem=string.lower(item)
                    #debugLog("doDirectory(): lowitem: {0}".format(lowitem))
                    if lowitem.endswith(extensionList)==True:
                        itemlist.append(('',item,'file',mtime))
        
        debugLog("doDirectory(): Order: {0}".format(order))
        debugLog("doDirectory(): Sort: {0}".format(sort))
        debugLog("doDirectory(): Heira: {0}".format(hierarchy))

        #for item in itemlist:
        #    debugLog("doDirectory(): In: {0}".format(item))

        if rootlevel==False:
            debugLog("doDirectory(): Sorting")
            
            # Call sorting on the list
            itemlist = sortList(itemlist, sort, order)

        #for item in itemlist:
        #    debugLog("doDirectory(): Out: {0}".format(item))
        
        # Now dump the sorted table
        response=response+"["
        loop=0

        while loop<len(itemlist):
            ident,name,type,mtime=itemlist[loop]

            #Try to decode what we think is garbage to UTF-8, if the decode fails, then we know it is a bad filename and will prefix it with ############
            try:
                ucode=unicode( name, "utf-8" )
                name=ucode.encode('utf-8', 'xmlcharrefreplace')
            except:
                stripped = (c for c in name if 0 < ord(c) < 127)
                name='#################' + ''.join(stripped)

            if ident:
                jsonitem=json.dumps({"id":str(loop), "name": name, "type": "folder"})
            else:
                jsonitem=json.dumps({"name": name, "type": type})
            if loop>0: response=response+","
            debugLog("doDirectory(): Out: [{0}] {1}".format(loop,jsonitem))
            response=response+jsonitem
            loop=loop+1
        response=response+"]"
        #debugLog("doDirectory(): Response: {0}".format(response))

        if len(response)>0:
            chocchip=Cookie.SimpleCookie()
            chocchip[COOKIENAME]=sessions[client]['hash']
            self.send_response(200)
            self.send_header('Set-Cookie',chocchip.output(header=''))
            self.send_header('Servetome-Version',STM_VERSION)
            self.send_header('Connection','close')
            self.send_header('Json-Length',len(response))
            self.send_header('Content-Length',len(response))
            self.send_header('Content-Type','application/json')
            self.end_headers()
            self.wfile.write(response)
        else:
            self.send_error(500,'Empty Response for: %s' % self.path)

        return

    except IOError:
        self.send_error(500,'Decoding error on: %s' % self.path)


#Recursively find files in a given directory
def listFiles(basePath, dir):
    debugLog("listFile(): Base=" +basePath +"   Dir=" + dir)

    returnList=[]
    
    if os.path.isdir(basePath + "/" + dir) == True:
        basedir = basePath + "/" + dir
        subdirlist = []
        for item in os.listdir(basedir):
            if os.path.isfile(item):
                debugLog("File Item Prefetch =" + item)

                # If this file extension is supported, then add this relative file path to the list
                if isReadableFile(item):
                    mtime=os.stat(basedir + "/" + item).st_mtime
                    #returnList.append(('', dir + "/" + item, 'absolutePath', mtime))
                    returnList.append(('', dir + "/" + item, 'file', mtime))
            else:
                subdirlist.append(os.path.join(dir, item))
                
        for subdir in subdirlist:
            subList=[]
            subList=listFiles(basePath, subdir)
            returnList.extend(subList)

    # If this is not a directory, we assume it is a file and add this relative file path to the list
    # "absolutePath" assumes the path you start out with is the root path.
    else:
        mtime=os.stat(basePath + "/" + dir).st_mtime
        fileName = os.path.basename(basePath + "/" + dir)
        debugLog("File Item =" + fileName)
        if isReadableFile(fileName):
            #returnList.append(('', dir, 'absolutePath', mtime))
            returnList.append(('', dir, 'file', mtime))

    return returnList


#Given a list, sort by type and then by order
def sortList(listToSort, sortType, sortOrder):
    class Sort:
        ident=0            
        name=1
        type=2
        date=3
        none=4

    class Order:
        none=0
        ascending=1
        descending=2

    if sortType==Sort.name:
        listToSort=sorted(listToSort, key=itemgetter(1))
        listToSort=sorted(listToSort, key=itemgetter(2), reverse=True)
    elif sortType==Sort.date:
        listToSort=sorted(listToSort, key=itemgetter(3), reverse=True)

    if sortOrder==Order.descending:
        listToSort.reverse()

    return listToSort


#Test to see if the filename are ones supported by StreamToMe
def isReadableFile(fileName):
    lowitem=string.lower(fileName)
    return lowitem.endswith(extensionList)==True
        


        
class requestHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        global commandHandle
        purl=urlparse(self.path)
        if not commandHandle is None: commandHandle.write("{0}\n".format(self.path))
        debugLog("requestHandler(): Client:   {0}".format(self.client_address[0]+":"+str(self.client_address[1])))
        if purl.scheme:   debugLog("requestHandler(): Scheme:   {0}".format(purl.scheme))
        if purl.netloc:   debugLog("requestHandler(): Netloc:   {0}".format(purl.netloc))
        if purl.path:     debugLog("requestHandler(): Path:     {0}".format(purl.path))
        if purl.params:   debugLog("requestHandler(): Params:   {0}".format(purl.params))
        if purl.query:    debugLog("requestHandler(): Query:    {0}".format(purl.query))
        if purl.fragment: debugLog("requestHandler(): Fragment: {0}".format(purl.fragment))
  
        options=purl.query.split("&")
        url=urllib.unquote(purl.path).split('/')
        
        # Delete anything prefixing the root
        if len(url)>0: del url[0]

        # Extract the command name and strip it from the url
        command=""
        if len(url)>0:
            command=url[0]
            del url[0]

        # Attempt to extract cookie from the headers and determine the client session
        client=""
        if "Cookie" in self.headers:
            chocchip=Cookie.SimpleCookie(self.headers["Cookie"])
            client=chocchip[COOKIENAME].value
            debugLog("requestHandler(): Cookie:   {0}".format(client))
            
            # Does this session exist?
            if not sessions.has_key(client):
                debugLog("requestHandler(): SessionID:{0}, doesn't exist, creating".format(client))                
                client=sessionNew(client)
        else:
            if command=="":
                # Special case of an empty command, we dont do cookies for this
                debugLog("requestHandler(): No Cookie, empty command")
            else:
                client=sessionNew()
                debugLog("requestHandler(): New session {0}".format(client))                
        
        override=True
        # Check client authentication state
        if override or client=="" or sessions[client]['authenticated']:
            if command=="stream":
                doStream(client,self,url,options)
            elif command=="folders" or command=="contents":
                doDirectory(client,self,url,options)
            elif command=="metadata":
                doMetadata(client,self,url,options)
            else:
                doRoot(self,url,options)
        else:
            # Digest username=\"user\",
            # realm=\"serveToMe\",
            # nonce=\"6c2ae85c-1d69-4942-b40f-c8b65a1587e9\",#
            # uri=\"/contents/2/dir1?sort=name&order=ascending&hierarchy=folder\",
            # response=\"5472bed9ee9d18ba85a2d65afc6a51a0\",
            # cnonce=\"30f526da4a368395443890ae9bf4b042\",
            # nc=00000001,
            # qop=\"auth\"
            nonce=genHash()
            authresp="Digest realm=\"serveToMe\",qop=\"auth\",nonce=\""+nonce+"\""
            response="Please login"
            chocchip=Cookie.SimpleCookie()
            chocchip[COOKIENAME]=sessions[client]['hash']
            self.send_response(401)
            self.send_header('Set-Cookie',chocchip.output(header=''))
            self.send_header('Servetome-Version',STM_VERSION)
            self.send_header('Connection','close')
            self.send_header('WWW-Authenticate',authresp)
            self.send_header('Content-Length',len(response))
            self.send_header('Content-Type','text/html')
            self.end_headers()
            self.wfile.write(response)
        
        return

def sigHandler(signum, frame):
    debugLog('Caught signal {0}, throwing keyboard interrupt'.format(signum))
    raise KeyboardInterrupt
                
def main():
    global username
    global password
    global listenPort
    global tempDir
    global execDir
    global directoryList
    global extensionList
    global commandHandle
    global debugHandle
    global debugEnable

    signal.signal(signal.SIGTERM, sigHandler)

    try:
        cfgfile=sys.path[0]+"/stm.cfg"
        if sys.platform=='win32': cfgfile=cfgfile.replace('/','\\')

        config=ConfigParser.RawConfigParser()
        config.read(cfgfile)

        if config.has_option('ServeToMe','username'): username=config.get('ServeToMe','username')
        if config.has_option('ServeToMe','password'): password=config.get('ServeToMe','password')
        if config.has_option('ServeToMe','listenPort'): listenPort=config.getint('ServeToMe','listenPort')
        tempDir=config.get('ServeToMe','tempDir')
        execDir=config.get('ServeToMe','execDir')
        directoryList=config.get('ServeToMe','directoryList').split(',')
        if config.has_option('ServeToMe','extensionList'): extensionList=tuple(config.get('ServeToMe','extensionList').split(','))
        if config.has_option('ServeToMe','debugEnable'): debugEnable=config.getboolean('ServeToMe','debugEnable')
    except:
        print "Configuration File Error\n"
        sys.exit(2)

    if debugEnable:
        if commandHandle is None: commandHandle=open(tempDir+"/command.log","w")
        if debugHandle is None: debugHandle=open(tempDir+"/debug.log","w")
        #sys.stderr = debugHandle
    else:
        sys.stderr = open(os.path.devnull, 'a+', 0)

    debugLog("Welcome to ServeToMe ({0})".format(STM_VERSION))
    debugLog("====================")

    debugLog("username={0}".format(username))
    debugLog("password={0}".format(password))
    debugLog("listenPort={0}".format(listenPort))
    debugLog("tempDir={0}".format(tempDir))
    debugLog("execDir={0}".format(execDir))
    debugLog("directoryList={0}".format(directoryList))
    debugLog("extensionList={0}".format(extensionList))
    debugLog("debugEnable={0}".format(debugEnable))

    try:
        server = HTTPServer(('', listenPort), requestHandler)
        itimer = RepeatTimer(SESSION_TIMEOUT_STEP, sessionIdle)
        itimer.start()
        server.protocol_version='HTTP/1.1'
        debugLog('started httpserver...')
        server.serve_forever()
    except KeyboardInterrupt:
        debugLog('Termination signal received, shutting down server')
        itimer.cancel()
        server.socket.close()
        while len(sessions)>0:
            for client in sessions:
                sessionKill(client)
                break
        # Close debug command log
        if not commandHandle is None: commandHandle.close()
        if not debugHandle is None: debugHandle.close()

if __name__ == '__main__':
    main()

