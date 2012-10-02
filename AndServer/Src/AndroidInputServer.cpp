/*

   AndServer, part of Android™ Virtual Input for Linux project

   Copyright 2012 Piotr Zintel
   zintelpiotr@gmail.com

   Android is a trademark of Google Inc.

*/

/*

   AndServer is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   any later version.

   AndServer is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "AndroidInputServer.h"

AndroidInputServer::AndroidInputServer() {
	keyboardListeningPort = 0;
	mouseListeningPort = 0;
	maxConnections = 0;
	activeConnections = 0;
	mouseListeningSocket = 0;
	keyboardListeningSocket = 0;
	clientSocket = 0;
	isDaemon = true;
	debug = false;
	child = false;
	logger = new Logger();
	optionsFilePath = strcpy(new char[strlen(OPTIONS_DEFAULT_FILE_PATH) + 1], OPTIONS_DEFAULT_FILE_PATH);
	sslCertificateFile = NULL;
	sslPrivateKeyFile = NULL;
	keyboardFilePath = NULL;
	mouseFilePath = NULL;
	mouseSem = NULL;
	keyboardSem = NULL;
	mouseSemName = NULL;
	keyboardSemName = NULL;
	keyboardClientHandler = NULL;
	mouseClientHandler = NULL;
	programName = NULL;
}

AndroidInputServer::~AndroidInputServer(){
	if (mouseListeningSocket) close(mouseListeningSocket);
	if (keyboardListeningSocket) close(keyboardListeningSocket);
	if ((mouseSem) && (mouseSemName)) {
		sem_close(mouseSem);
		if (!child) {
			sem_unlink(mouseSemName);
		}
		delete[] mouseSemName;
	}
	if ((keyboardSem) && (keyboardSemName)) {
		sem_close(keyboardSem);
		if (!child) {
			sem_unlink(keyboardSemName);
		}
		delete[] keyboardSemName;
	}
	if (sslCertificateFile ) delete[] sslCertificateFile;
	if (sslPrivateKeyFile ) delete[] sslPrivateKeyFile;
	if ( (logger) ) delete logger;
	if (keyboardClientHandler) delete keyboardClientHandler;
	if (mouseClientHandler) delete mouseClientHandler;
	if (programName) delete[] programName;
	if (keyboardFilePath) delete[] keyboardFilePath;
	if (mouseFilePath) delete[] mouseFilePath;
	if (optionsFilePath) delete[] optionsFilePath;
}

bool AndroidInputServer::initialize(int argc, char* argv[]) {
	// parse arguments, daemonize, create device handlers, ready sockets

	if ( !parseOptions(argc, argv) ) {
		logger->error("Error parsing parametres");
		return false;
	}

	if ( !parseOptionsFile() ) {
		logger->error("Error in config file");
		return false;
	}

	if ( !getDefaultPaths() ) {
		logger->error("Error setting options");
		return false;
	}

	if (debug) {
		logger->printMessage("08.12.2011 16:25:02 Options parsed");
	}

	if (!semaphoresInit()) {
		logger->error("13.12.2011 21:17:54 Semaphore initialization failed");
		return false;
	}

	if ( isDaemon ) {
		if (!daemonize()) {
			logger->error("06.12.2011 01:34:59 daemonize() error");
			return false;
		}
	}

	if (isDaemon && debug) {
		logger->printMessage("06.12.2011 01:40:39 Daemonized");
	}

	if ( !readySockets() ) {
		logger->error("08.12.2011 16:20:59 readySockets() error");
		return false;
	}

	if (debug) {
		logger->printMessage("08.12.2011 16:27:10 listeningSocket bound");
	}

	return true;
}

bool AndroidInputServer::andListen() {
	logger->printMessage("Android input server running");
	int listeningSocket;
	int listeningPort;
	char tmp[128];
	int ret, ndfs;
	fd_set rfds;
	sigset_t emptymask;
	sigemptyset(&emptymask);

	ndfs = mouseListeningSocket > keyboardListeningSocket ? mouseListeningSocket + 1 : keyboardListeningSocket + 1;

	sprintf(tmp,"waiting for a mouse connection on port: %d",mouseListeningPort);
	logger->printMessage(tmp);
	sprintf(tmp,"waiting for a keyboard connection on port: %d",keyboardListeningPort);
	logger->printMessage(tmp);

	for(;;) {
		if (receivedEndSignal) {
			if (debug) {
				logger->printMessage("12.12.2011 00:10:45 Received end signal. Will quit");
			}
			if (!child) {
				logger->printMessage("Received end signal. Server will exit after closing existing connections.");
			}
			kill(0,SIGTERM);
			return true;
		}

		if ( activeConnections >= maxConnections ) {
			sigset_t mask, oldmask;
			sigemptyset(&mask);
			sigaddset(&mask, SIGTERM | SIGINT | SIGCHLD);
			sigsuspend(&oldmask);
			sigprocmask(SIG_UNBLOCK, &mask, NULL);
			continue;
		}

		int addrlen = sizeof(clientAddress);
		memset(&clientAddress,0,sizeof(clientAddress) );
		
		FD_ZERO(&rfds);
		FD_SET(mouseListeningSocket, &rfds);
		FD_SET(keyboardListeningSocket, &rfds);

		if ( (ret = pselect(ndfs,&rfds,NULL,NULL,NULL,&emptymask)) < 0 ) {
			if (errno != EINTR) logger->error("27.03.2012 18:27:18 select() error",errno);
		} else { 
			for (int i = 0; i < 2; ++i) {
				if (i == 0 && FD_ISSET(keyboardListeningSocket, &rfds)) {
					listeningSocket = keyboardListeningSocket;
					listeningPort = keyboardListeningPort;
				} else if (i == 1 && FD_ISSET(mouseListeningSocket, &rfds)) {
					listeningSocket = mouseListeningSocket;
					listeningPort = mouseListeningPort;
				}

				clientSocket = accept(listeningSocket, (struct sockaddr*)&clientAddress, (socklen_t *)&addrlen);

				if (receivedSigChild) {
					receivedSigChild = false;
					continue;
				}

				if ( (clientSocket == -1) && !receivedEndSignal ) {
					if (errno != EAGAIN) logger->error("08.12.2011 20:23:57 accept() error",errno);
					continue;
				} else if (receivedEndSignal) {
					if (debug) {
						logger->printMessage("12.12.2011 00:10:40 Received end signal. Will quit");
					}
					if (!child) {
						logger->printMessage("Received end signal. Server will exit after closing existing connections.");
					}
					return true;
				}

				int pid = handleConnectionRequest(listeningSocket);

				if ( pid == 0 ) {
					return true;
				}

				if ( pid == 1 ) {
					char tmp[128];
					sprintf(tmp,"13.12.2011 11:57:48 Connection accepted on port %d",listeningPort);
					logger->printMessage(tmp);
					activeConnections++;
					usleep(500);
					continue;
				} else {
					logger->error("13.12.2011 23:00:42 handleConnectionRequest() error");
					continue;
				}
			}
		}
	}
}

int AndroidInputServer::handleConnectionRequest(const int acceptedFromListeningSocket) {
	pid_t pid = fork();
	if ( pid == -1 ) {
		logger->error("08.12.2011 23:28:27 fork() error",errno);
		return -1;
	}
	if ( pid == 0 ) { // child
		child = true;
		close(keyboardListeningSocket);
		close(mouseListeningSocket);
		handleClient(acceptedFromListeningSocket);
		return 0;
	}
	// parent
	close(clientSocket);
	return 1;
}

void AndroidInputServer::handleClient(const int acceptedFromListeningSocket){
	if (acceptedFromListeningSocket == keyboardListeningSocket) {
		keyboardClientHandler = new KeyboardClientHandler(clientSocket, logger, keyboardSemName, sslCertificateFile, sslPrivateKeyFile, keyboardFilePath);
		keyboardClientHandler->handleClient();
		delete keyboardClientHandler;
		keyboardClientHandler = NULL;
	} else {
		mouseClientHandler = new MouseClientHandler(clientSocket, logger, mouseSemName, sslCertificateFile, sslPrivateKeyFile, mouseFilePath);
		mouseClientHandler->handleClient();
		delete mouseClientHandler;
		mouseClientHandler = NULL;
	}
}

void AndroidInputServer::handleEndSignal(int s) {
	receivedEndSignal = true;
}

void AndroidInputServer::handleSigChild(int s)
{
	while ( waitpid(-1, 0, WNOHANG) > 0 )
	activeConnections --;
	receivedSigChild = true;
}

int AndroidInputServer::splitServer() {
	pid_t pid = fork();
	if (pid == -1) {
		char tmp[128];
		sprintf(tmp,"15.12.2011 07:55:35 fork() error: (%d) %s",errno,strerror(errno));
		logger->error(tmp);
		return -1;
	}
	if (pid == 0) { // child, mouseServer listening process
		child = true;
		close(keyboardListeningSocket);
		return 0;
	} // parent, keyboardServer listening process
	close(mouseListeningSocket);
	return 1;
}

void AndroidInputServer::usage(const char* programName) {
	cerr << "\nUsage: " << programName << " [-s] [-d] [-h] [-o CONFIGFILE] [-k KPORT] [-m MPORT] [-l MAXCONNECTIONS] [-C CERTFILE] [-P KEYFILE] [-M MSFILE] [-K KBDFILE]\n\n";
	cerr << "-s\tdo not run as daemon (all messages will be printed to stderr instead of syslog)\n";
	cerr << "-d\tturns debug output on\n";
	cerr << "-h\tdisplay this text\n";
	cerr << "-o\tuse [CONFIGFILE] to read configuration from\n";
	cerr << "-k\tchange keyboard listening port to KPORT";
	cerr << " (default port is " << KEYBOARD_DEFAULT_PORT << ")\n";
	cerr << "-m\tchange mouse listening port to MPORT";
	cerr << " (default port is " << MOUSE_DEFAULT_PORT << ")\n";
	cerr << "-l\tset max connection";
	cerr << " (default value is 2)\n";
	cerr << "-C\tset SSL certificate path\n";
	cerr << "-P\tset SSL private key path\n";
	cerr << "-M\tset mouse driver dev node path (default is \"/dev/avms\")\n";
	cerr << "-K\tset keyboard driver dev node path (default is \"/dev/avkbd\")\n";
	cerr << "\nYou can close the program by sending SIGINT or SIGKILL (kill PID).\n";
	exit(1);
}

bool AndroidInputServer::daemonize() {
	int i;
	pid_t pid;
	openlog(programName, LOG_PID, LOG_DAEMON);

	pid = fork();
	if (pid == -1) {
		logger->error("06.12.2011 01:35:19 fork() error");
		return false;
	}

	if (pid != 0) {
		exit(EXIT_SUCCESS);
	}

	if (debug && (pid != 0) ) {
		logger->printMessage("06.12.2011 01:35:36 parent not closed");
	}
	if (debug && (pid == 0) ) {
		logger->printMessage("06.12.2011 01:35:42 child: parent closed");
	}

	if ( setsid() == -1 ) {
		logger->error("06.12.2011 01:36:52 setsid() error");
		return false;
	}

	signal(SIGHUP,SIG_IGN);
	if ( (pid=fork()) == -1 ) {
		logger->error("06.12.2011 01:37:00 fork() error");
		return false;
	}
	if ( pid != 0 ) {
		exit(EXIT_SUCCESS);
	}

	if (debug && (pid != 0) ) {
		logger->printMessage("06.12.2011 01:37:59 parent not closed");
	}
	if (debug && (pid == 0) ) {
		logger->printMessage("06.12.2011 01:38:07 child: parent closed");
	}

	if ( setsid() == -1 ) {
		logger->error("06.12.2011 01:33:49 :setsid() error");
		return false;
	}

	if ( (chdir("/")) == -1 ) {
		logger->error("06.12.2011 01:38:36 chdir() error");
		return false;
	}

	umask(0);

	for (i=getdtablesize()-1; i>=0; --i)
		close(i);

	return true;
}

bool AndroidInputServer::readySockets(){
	char tmp[128];
	int *optval = new int(1);
	int flags;

	//========= keyboardSocket ==================
	keyboardListeningSocket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if ( keyboardListeningSocket == -1 ) {
		sprintf(tmp,"08.12.2011 16:21:36 socket() error: (%d) %s",errno, strerror(errno));
		logger->error(tmp);
		delete optval;
		return false;
	}
	memset(&keyboardServerAddress, 0, sizeof(keyboardServerAddress));
	keyboardServerAddress.sin_family = AF_INET;
	keyboardServerAddress.sin_addr.s_addr = INADDR_ANY;
	keyboardServerAddress.sin_port = htons(keyboardListeningPort);

	if ( bind(keyboardListeningSocket, (struct sockaddr *)&keyboardServerAddress,sizeof(keyboardServerAddress)) == -1 ) {
		sprintf(tmp,"08.12.2011 16:21:55 bind() error. Perhaps the port numbers are too low? Use >=1024 if you are not root: (%d) %s",errno, strerror(errno));
		logger->error(tmp);
		delete optval;
		return false;
	}

	if ( listen(keyboardListeningSocket, maxConnections) == -1 ) {
		sprintf(tmp,"15.12.2011 08:02:43 Listen() error: (%d) %s",errno, strerror(errno));
		logger->error(tmp);
		delete optval;
		return false;
	}

	if (setsockopt(keyboardListeningSocket,SOL_SOCKET,SO_REUSEADDR,optval, sizeof(int)) == -1) {
		sprintf(tmp,"15.12.2011 23:06:25 setsockopt() error: (%d) %s",errno, strerror(errno));
		logger->error(tmp);
		delete optval;
		return false;
	}

	if ((flags = fcntl(keyboardListeningSocket, F_GETFL, 0)) < 0) 
	{ 
		logger->error("27.03.2012 20:58:26 fcntl()",errno);
		return false;
	}

	if (fcntl(keyboardListeningSocket, F_SETFL, flags | O_NONBLOCK) < 0) 
	{ 
		logger->error("27.03.2012 20:58:12 fcntl() ",errno);
		return false;
	} 

	//========= mouseSocket ==================
	mouseListeningSocket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if ( mouseListeningSocket == -1 ) {
		sprintf(tmp,"15.12.2011 08:02:48 socket() error: (%d) %s",errno, strerror(errno));
		logger->error(tmp);
		delete optval;
		return false;
	}
	memset(&mouseServerAddress, 0, sizeof(mouseServerAddress));
	mouseServerAddress.sin_family = AF_INET;
	mouseServerAddress.sin_addr.s_addr = INADDR_ANY;
	mouseServerAddress.sin_port = htons(mouseListeningPort);

	if ( bind(mouseListeningSocket, (struct sockaddr *)&mouseServerAddress,sizeof(mouseServerAddress)) == -1 ) {
		sprintf(tmp,"15.12.2011 08:02:57 bind() error. Perhaps the port numbers are too low? Use >=1024 if you are not root: (%d) %s",errno, strerror(errno));
		logger->error(tmp);
		delete optval;
		return false;
	}

	if ( listen(mouseListeningSocket, maxConnections) == -1 ) {
		sprintf(tmp,"15.12.2011 08:03:04 Listen() error: (%d) %s",errno, strerror(errno));
		logger->error(tmp);
		delete optval;
		return false;
	}

	if (setsockopt(mouseListeningSocket,SOL_SOCKET,SO_REUSEADDR,optval, sizeof(int)) == -1) {
		sprintf(tmp,"15.12.2011 23:11:43 setsockopt() error: (%d) %s",errno, strerror(errno));
		logger->error(tmp);
		delete optval;
		return false;
	}

	if ((flags = fcntl(mouseListeningSocket, F_GETFL, 0)) < 0) 
	{ 
		logger->error("27.03.2012 20:56:08 fcntl()",errno);
		return false;
	}

	if (fcntl(mouseListeningSocket, F_SETFL, flags | O_NONBLOCK) < 0) 
	{ 
		logger->error("27.03.2012 20:57:21 ",errno);
		return false;
	}

	delete optval;
	return true;
}

bool AndroidInputServer::semaphoresInit() {
	mouseSemName = new char[16];
	keyboardSemName = new char[16];
	memcpy(mouseSemName,"AvmsSem",strlen("AvmsSem") + 1);
	memcpy(keyboardSemName,"AvkbdSem",strlen("AvkbdSem") + 1);

	mouseSem = sem_open(mouseSemName,O_CREAT,S_IWUSR|S_IRUSR,1);
	if (mouseSem == SEM_FAILED) {
		char tmp[128];
		sprintf(tmp,"13.12.2011 21:26:53 sem_open() failed. Errno: (%d) %s",errno, strerror(errno));
		logger->error(tmp);
		return false;
	}

	keyboardSem = sem_open(keyboardSemName,O_CREAT,S_IWUSR|S_IRUSR,1);
	if (mouseSem == SEM_FAILED) {
		char tmp[128];
		sprintf(tmp,"13.12.2011 21:30:06 sem_open() failed. Errno: (%d) %s",errno, strerror(errno));
		logger->error(tmp);
		return false;
	}

	return true;
}

bool AndroidInputServer::parseOptions(int argc, char* argv[]){
	int c;
	struct stat st;
	int ret;

	programName = new char[strlen(basename(argv[0])) + 1];
	programName = strncpy(programName,basename(argv[0]),strlen(basename(argv[0])));

	while((c = getopt(argc, argv, "sdho:k:m:l:C:P:M:K:")) != -1)
	{
		switch (c)
		{
			case 's':
				isDaemon = false;
				logger->isDaemon = false;
			break;

			case 'd':
				debug = true;
			break;

			case 'h':
				usage(programName);

			case 'o':
				ret = stat(optarg, &st);
				if ( ret != -0 ) {
					cerr << "\n-o: \"" << optarg << "\" incorrect argument, file not found\n";
					usage(programName);
				} else if ( ( ret == 0 ) && ( !S_ISREG(st.st_mode) ) ) {
					cerr << "\n-o: \"" << optarg << "\" incorrect argument, not a regular file\n";
					usage(programName);
				} else {
					delete[] optionsFilePath;
					optionsFilePath = strcpy(new char[strlen(optarg) + 1],optarg);
				}
			break;

			case 'k':
				if ( isNumber(optarg) ) {
					if (mouseListeningPort != atoi(optarg) )
						keyboardListeningPort = atoi(optarg);
					else {
						cerr << "\n-k: " << optarg << " - mouse listening port cannot be the same as keyboard listening port\n";
						usage(programName);
					}
					break;
				}
				else {
					cerr << "\n-k: " << optarg << " is not a correct port number\n";
					usage(programName);
				}
			case 'm':
				if ( isNumber(optarg) ) {
					if (keyboardListeningPort != atoi(optarg) )
						mouseListeningPort = atoi(optarg);
					else {
						cerr << "\n-k: " << optarg << " - keyboard listening port cannot be the same as mouse listening port\n";
						usage(programName);
					}
					break;
				}
				else {
					cerr << "\n-m: " << optarg << " is not a correct port number\n";
					usage(programName);
				}
			case 'l':
				if ( isNumber(optarg) ) {
					maxConnections = atoi(optarg);
					break;
				}
				else {
					cerr << "\n-l: " << optarg << " is not a correct number\n";
					usage(programName);
				}
			case 'C':
				ret = stat(optarg, &st);
				if ( ret != -0 ) {
					cerr << "\n-C: \"" << optarg << "\" incorrect argument, file not found\n";
					usage(programName);
				} else if ( ( ret == 0 ) && ( !S_ISREG(st.st_mode) ) ) {
					cerr << "\n-C: \"" << optarg << "\" incorrect argument, not a regular file\n";
					usage(programName);
				} else {
					sslCertificateFile = strcpy(new char[strlen(optarg) + 1],optarg);
				}
			break;

			case 'P':
				ret = stat(optarg, &st);
				if ( ret != -0 ) {
					cerr << "\n-P: \"" << optarg << "\" incorrect argument, file not found\n";
					usage(programName);
				} else if ( ( ret == 0 ) && ( !S_ISREG(st.st_mode) ) ) {
					cerr << "\n-P: \"" << optarg << "\" incorrect argument, not a regular file\n";
					usage(programName);
				} else {
					sslPrivateKeyFile = strcpy(new char[strlen(optarg) + 1],optarg);
				}
			break;

			case 'M':
				ret = stat(optarg, &st);
				if ( ret != -0 ) {
					cerr << "\n-M: \"" << optarg << "\" incorrect argument, file not found\n";
					usage(programName);
				} else if ( ( ret == 0 ) && ( !S_ISCHR(st.st_mode) ) ) {
					cerr << "\n-M: \"" << optarg << "\" incorrect argument, not a character special file\n";
					usage(programName);
				} else {
					if ( mouseFilePath ) delete[] mouseFilePath;
					mouseFilePath = strcpy(new char[strlen(optarg) + 1],optarg);
				}
			break;

			case 'K':
				ret = stat(optarg, &st);
				if ( ret != -0 ) {
					cerr << "\n-K: \"" << optarg << "\" incorrect argument, file not found\n";
					usage(programName);
				} else if ( ( ret == 0 ) && ( !S_ISCHR(st.st_mode) ) ) {
					cerr << "\n-K: \"" << optarg << "\" incorrect argument, not a character special file\n";
					usage(programName);
				} else {
					if ( keyboardFilePath ) delete[] keyboardFilePath;
					keyboardFilePath = strcpy(new char[strlen(optarg) + 1],optarg);
				}
			break;

			default:
				usage(programName);
		}
	}
	return true;
}

bool AndroidInputServer::getDefaultPaths() {
	if (!keyboardFilePath) {
		keyboardFilePath = strcpy(new char[strlen(KEYBOARD_DEFAULT_FILE_PATH) + 1], KEYBOARD_DEFAULT_FILE_PATH);
		char tmp[128];
		sprintf(tmp,"Keyboard file path not specified, using default : %s",keyboardFilePath);
		logger->printMessage(tmp);
	}
	if (!mouseFilePath) {
		mouseFilePath = strcpy(new char[strlen(MOUSE_DEFAULT_FILE_PATH) + 1], MOUSE_DEFAULT_FILE_PATH);
		char tmp[128];
		sprintf(tmp,"Mouse file path not specified, using default : %s",mouseFilePath);
		logger->printMessage(tmp);
	}
	if (!sslCertificateFile) {
		logger->error("Error: certificate file not specified, will quit");
		return false;
	}
	if  (!sslPrivateKeyFile) {
		logger->error("Error: private key file not specified, will quit");
		return false;
	}

	if (maxConnections == 0) {
		maxConnections = 2;
		logger->printMessage("Maximum number of connections not specified, using default: 2");
	}
	if (mouseListeningPort == 0) {
		mouseListeningPort = MOUSE_DEFAULT_PORT;
		char tmp[128];
		sprintf(tmp,"Mouse listening port not specified, using default : %d",MOUSE_DEFAULT_PORT);
		logger->printMessage(tmp);
	}
	if (keyboardListeningPort == 0) {
		keyboardListeningPort = KEYBOARD_DEFAULT_PORT;
		char tmp[128];
		sprintf(tmp,"Keyboard listening port not specified, using default: %d",KEYBOARD_DEFAULT_PORT);
		logger->printMessage(tmp);
	}
	return true;
}

bool AndroidInputServer::parseOptionsFile() {
	int bufferLength = 512;
	FILE *optsFile = NULL;
	char *optsLineBuffer = new char[bufferLength];
	char *optName = new char[bufferLength];
	char *optArg = new char[bufferLength];
	int lineNo = 0;
	bool error = false;
	char *fret;

	if ( (optsFile = fopen(optionsFilePath, "r")) == NULL ) {
		char tmp[128];
		sprintf(tmp,"could not open options file %s",optionsFilePath);
		logger->error(tmp, errno);
		goto finalize;
	}

	while (!feof(optsFile)) {
		memset(optsLineBuffer,0,bufferLength);
		memset(optName,0,bufferLength);
		memset(optArg,0,bufferLength);
		
		fret = fgets(optsLineBuffer, bufferLength, optsFile);
		++lineNo;
		if ((fret != NULL ) ) {
			int ret = sscanf(optsLineBuffer, "%s%s", optName, optArg);
			if ( (ret) == EOF ) {
				if (ferror(optsFile)) {
					logger->error("03.03.2012 20:23:13; sscanf() error", errno);
					error = true;
					goto finalize;
				} else {
					continue;
				}
			} else if ( optsLineBuffer[0] == '#' ) {
				continue;
			} else if (ret < 2) {
				char tmp[128];
				sprintf(tmp,"Error in config file, line %d: \"%s %s\"",lineNo,optName,optArg);
				logger->error(tmp);
			} else { 
				if (strcmp(optName, "mouse-port") == 0 ) {
					int arg = 0;
					if ( !isNumber(optArg) || (sscanf(optArg,"%d", &arg) != 1) ) {
						char tmp[128];
						sprintf(tmp,"Error in config file, line %d: mouse-port: incorrect option argument",lineNo);
						logger->error(tmp);
					} else {
						if (mouseListeningPort == 0 ) {
							if ( arg == keyboardListeningPort ) {
								char tmp[128];
								sprintf(tmp,"Error in config file, line %d: mouse-port cannot be the same as keyboard port",lineNo);
								logger->error(tmp);
							} else  {
								mouseListeningPort = arg;
							}
						}
					}

				} else if (strcmp(optName, "keyboard-port") == 0) {
					int arg = 0;
					if ( !isNumber(optArg) || (sscanf(optArg,"%d", &arg) != 1) ) {
						char tmp[128];
						sprintf(tmp,"Error in config file, line %d: keyboard-port: incorrect option argument",lineNo);
						logger->error(tmp);
					} else {
						if (keyboardListeningPort == 0) {
							if ( arg == mouseListeningPort ) {
								char tmp[128];
								sprintf(tmp,"Error in config file, line %d: keyboard-port cannot be the same as mouse port",lineNo);
								logger->error(tmp);
							} else  {
								keyboardListeningPort = arg;
							}
						}
					}

				} else if (strcmp(optName, "max-connections") == 0) {
					int arg = 0;
					if ( !isNumber(optArg) || (sscanf(optArg,"%d", &arg) != 1) ) {
						char tmp[128];
						sprintf(tmp,"Error in config file, line %d: max-connections: incorrect option argument",lineNo);
						logger->error(tmp);
					} else {
						if (maxConnections == 0) maxConnections = arg;
					}

				} else if (strcmp(optName, "private-key-file") == 0) {
					char *arg = new char[strlen(optArg) + 1];
					if ( sscanf(optArg,"%s", arg) != 1 ) {
						char tmp[128];
						sprintf(tmp,"Error in config file, line %d: private-key-file: incorrect option argument",lineNo);
						logger->error(tmp);
					} else {
						if (!sslPrivateKeyFile) {
							struct stat st;
							int r = stat(optArg, &st);
							if ( r != 0 ) {
								char tmp[128];
								sprintf(tmp,"Error in config file, line %d: \"%s\" - private key file not found",lineNo, optArg);
								logger->error(tmp);
							} else if ( (r == 0) && ( !S_ISREG(st.st_mode) ) ) {
								char tmp[128];
								sprintf(tmp,"Error in config file, line %d: \"%s\" is not a regular file",lineNo, optArg);
								logger->error(tmp);							
							} else {
								sslPrivateKeyFile = strcpy(new char[strlen(arg) + 1], arg);
							}
						}
					}
					delete[] arg;

				} else if (strcmp(optName, "certificate-file") == 0) {
					char *arg = new char[strlen(optArg) + 1];
					if ( sscanf(optArg,"%s", arg) != 1 ) {
						char tmp[128];
						sprintf(tmp,"Error in config file, line %d: certificate-file: incorrect option argument",lineNo);
						logger->error(tmp);
					} else {
						if (!sslCertificateFile) {
							struct stat st;
							int r = stat(optArg, &st);
							if ( r != 0 ) {
								char tmp[128];
								sprintf(tmp,"Error in config file, line %d: \"%s\" - certificate file not found",lineNo, optArg);
								logger->error(tmp);
							} else if ( ( r == 0 ) && ( !S_ISREG(st.st_mode) ) ) {
								char tmp[128];
								sprintf(tmp,"Error in config file, line %d: \"%s\" is not a regular file",lineNo, optArg);
								logger->error(tmp);							
							} else {
								sslCertificateFile = strcpy(new char[strlen(arg) + 1], arg);
							}
						}
					}
					delete[] arg;

				} else if (strcmp(optName, "mouse-device-file") == 0) {
					char *arg = new char[strlen(optArg) + 1];
					if ( sscanf(optArg,"%s", arg) != 1 ) {
						char tmp[128];
						sprintf(tmp,"Error in config file, line %d: mouse-device-file: incorrect option argument",lineNo);
						logger->error(tmp);
					} else {
						if (!mouseFilePath) {
							struct stat st;
							int r = stat(optArg, &st);
							if ( r != 0 ) {
								char tmp[128];
								sprintf(tmp,"Warning (config file, line %d): \"%s\" - file not found, will be created upon a successful connection",lineNo,optArg);
								logger->error(tmp);
								mouseFilePath = strcpy(new char[strlen(arg) + 1], arg);
							} else if ( ( r == 0 ) && ( !S_ISCHR(st.st_mode) ) ) {
								char tmp[128];
								sprintf(tmp,"Error in config file, line %d: \"%s\" is not a character special file",lineNo, optArg);
								logger->error(tmp);
							}
						} 
					}
					delete[] arg;

				} else if (strcmp(optName, "keyboard-device-file") == 0) {
					char *arg = new char[strlen(optArg) + 1];
					if ( sscanf(optArg,"%s", arg) != 1 ) {
						char tmp[128];
						sprintf(tmp,"keyboard-device-file: incorrect option argument in config file, line %d",lineNo);
						logger->error(tmp);
					} else {
						if (!keyboardFilePath) {
							struct stat st;
							int r = stat(optArg, &st);
							if ( r != 0 ) {
								char tmp[128];
								sprintf(tmp,"Warning (config file, line %d): \"%s\" - file not found, will be created upon a successful connection",lineNo,optArg);
								logger->error(tmp);
								keyboardFilePath = strcpy(new char[strlen(arg) + 1], arg);
							} else if ( ( r == 0 ) && ( !S_ISCHR(st.st_mode) ) ) {
								char tmp[128];
								sprintf(tmp,"Error in config file, line %d: \"%s\" is not a character special file",lineNo, optArg);
								logger->error(tmp);							
							}
						}
					}
					delete[] arg;

				} else {
					char tmp[128];
					sprintf(tmp,"Error in config file, line %d (unknown option): \"%s\"",lineNo,optName);
					logger->error(tmp);
				}
			}
		} else if (ferror(optsFile)) {
			logger->error("03.03.2012 20:20:34 error parsing config file", errno);
			error = true;
			goto finalize;
		}
	}

	finalize:
	if (optsFile) {
		fclose(optsFile);
	}
	delete[] optArg;
	delete[] optName;
	delete[] optsLineBuffer;
	
	return !error;
}

bool AndroidInputServer::isNumber(const char * string)
{
	int i = 0;
	for (i = 0; string[i] != 0; ++i)
	{
		if (string[i] < '0' || string[i] > '9')
		{
			return false;
		}
	}
	return true;
}	
