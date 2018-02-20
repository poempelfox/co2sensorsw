/* $Id: co2sensord.c $
 * This is software for serving the data received from a certain type of
 * (relatively) cheap CO2 Sensors with USB connection to the network.
 * The device shows up on the USB as a HID, and you need to communicate
 * with it through the corresponding hidraw-device in Linux.
 * Big parts of this are copied from the hostsoftware of my various
 * temperature- and humidity-sensor-projects.
 * Figuring out how the sensors communicate via USB was luckily done by other
 * people, so I had lots of code to learn from. In particular, both
 * https://hackaday.io/project/5301-reverse-engineering-a-low-cost-usb-co-monitor/log/17909-all-your-base-are-belong-to-us
 * and https://github.com/vshmoylov/libholtekco2/
 * served as inspiration.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>  /* According to POSIX.1-2001 */
#include <termios.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>

int verblev = 1;
#define VERBPRINT(lev, fmt...) \
        if (verblev > lev) { \
          printf(fmt); \
          fflush(stdout); \
        }
int runinforeground = 0;
unsigned char * hidrawdevicepath = "/dev/hidraw9";
int restartonerror = 0;
/* The device does some weird pseudo-crypto with the transmitted data.
 * The following is just a random key we generated ourselves for that.
 * It is sent when requesting data from the device, and the device then
 * encrypts the data with it. Whatever that is supposed to be good for
 * on an USB link is beyond me... */
unsigned char keyforpseudocrypto[8] = { 0xa9, 0xd2, 0x22, 0x7f,
                                        0x04, 0xbc, 0x93, 0x99 };
double lasttemp = -274.0;
double lasthum = -1.0;
unsigned long lastco2 = 65533;

struct daemondata {
  uint16_t port;
  int fd;
  unsigned char outputformat[1000];
  struct daemondata * next;
};

static void usage(char *name)
{
  printf("usage: %s [-v] [-q] [-d n] [-h] command <parameters>\n", name);
  printf(" -v     more verbose output. can be repeated numerous times.\n");
  printf(" -q     less verbose output. using this more than once will have no effect.\n");
  printf(" -d p   Path of hidraw device (default: %s)\n", hidrawdevicepath);
  printf(" -f     relevant for daemon mode only: run in foreground.\n");
  printf(" -h     show this help\n");
  printf("Valid commands are:\n");
  printf(" daemon   Daemonize and answer queries. This requires one or more\n");
  printf("          parameters in the format\n");
  printf("           port[:outputformat]\n");
  printf("          port is a TCP port where the data from this sensor is to be served\n");
  printf("          The optional outputformat specifies how the\n");
  printf("          output to the network should look like.\n");
  printf("          Available are: %%T = temperature, %%H = humidity (not available on\n");
  printf("          all models), %%C = CO2. The default is '%%C %%T'.\n");
  printf("          Examples: '31337'   '7777:%%T %%C'\n");
}

void sigpipehandler(int bla) { /* Dummyhandler for catching the event */
  return;
}

void logaccess(struct sockaddr * soa, int soalen, char * txt) {
  struct sockaddr_in * sav4;
  struct sockaddr_in6 * sav6;

  if (soalen == sizeof(struct sockaddr_in6)) {
    sav6 = (struct sockaddr_in6 *)soa;
    if ((sav6->sin6_addr.s6_addr[ 0] == 0)
     && (sav6->sin6_addr.s6_addr[ 1] == 0)
     && (sav6->sin6_addr.s6_addr[ 2] == 0)
     && (sav6->sin6_addr.s6_addr[ 3] == 0)
     && (sav6->sin6_addr.s6_addr[ 4] == 0)
     && (sav6->sin6_addr.s6_addr[ 5] == 0)
     && (sav6->sin6_addr.s6_addr[ 6] == 0)
     && (sav6->sin6_addr.s6_addr[ 7] == 0)
     && (sav6->sin6_addr.s6_addr[ 8] == 0)
     && (sav6->sin6_addr.s6_addr[ 9] == 0)
     && (sav6->sin6_addr.s6_addr[10] == 0xFF)
     && (sav6->sin6_addr.s6_addr[11] == 0xFF)) {
      /* This is really a IPv4 not a V6 access, so log it as
       * a such. */
      VERBPRINT(2, "%d.%d.%d.%d\t%s\n", sav6->sin6_addr.s6_addr[12],
              sav6->sin6_addr.s6_addr[13],
              sav6->sin6_addr.s6_addr[14],
              sav6->sin6_addr.s6_addr[15], txt);
    } else {
      /* True IPv6 access */
      VERBPRINT(2, "%x:%x:%x:%x:%x:%x:%x:%x\t%s\n",
              (sav6->sin6_addr.s6_addr[ 0] << 8) | sav6->sin6_addr.s6_addr[ 1],
              (sav6->sin6_addr.s6_addr[ 2] << 8) | sav6->sin6_addr.s6_addr[ 3],
              (sav6->sin6_addr.s6_addr[ 4] << 8) | sav6->sin6_addr.s6_addr[ 5],
              (sav6->sin6_addr.s6_addr[ 6] << 8) | sav6->sin6_addr.s6_addr[ 7],
              (sav6->sin6_addr.s6_addr[ 8] << 8) | sav6->sin6_addr.s6_addr[ 9],
              (sav6->sin6_addr.s6_addr[10] << 8) | sav6->sin6_addr.s6_addr[11],
              (sav6->sin6_addr.s6_addr[12] << 8) | sav6->sin6_addr.s6_addr[13],
              (sav6->sin6_addr.s6_addr[14] << 8) | sav6->sin6_addr.s6_addr[15],
              txt);
    }
  } else if (soalen == sizeof(struct sockaddr_in)) {
    unsigned char brokeni32[4];

    sav4 = (struct sockaddr_in *)soa;
    brokeni32[0] = (sav4->sin_addr.s_addr & 0xFF000000UL) >> 24;
    brokeni32[1] = (sav4->sin_addr.s_addr & 0x00FF0000UL) >> 16;
    brokeni32[2] = (sav4->sin_addr.s_addr & 0x0000FF00UL) >>  8;
    brokeni32[3] = (sav4->sin_addr.s_addr & 0x000000FFUL) >>  0;
    VERBPRINT(2, "%d.%d.%d.%d\t%s\n", brokeni32[0], brokeni32[1],
            brokeni32[2], brokeni32[3], txt);
  } else {
    VERBPRINT(2, "!UNKNOWN_ADDRESS_TYPE!\t%s\n", txt);
  }
}


static void printtooutbuf(char * outbuf, int oblen, struct daemondata * dd) {
  unsigned char * pos = &dd->outputformat[0];
  while (*pos != 0) {
    if (*pos == '%') {
      pos++;
      if        ((*pos == 'T') || (*pos == 't')) { /* Temperature */
        if (lasttemp < -273.0) { /* no data yet */
          outbuf += sprintf(outbuf, "%s", "N/A");
        } else {
          if (*pos == 'T') { /* fixed width */
            outbuf += sprintf(outbuf, "%6.2lf", lasttemp);
          } else { /* variable width. */
            outbuf += sprintf(outbuf, "%.2lf", lasttemp);
          }
        }
      } else if ((*pos == 'H') || (*pos == 'h')
              || (*pos == 'F') || (*pos == 'f')) { /* Humidity */
        if (lasthum < 0.0) { /* no data yet */
          outbuf += sprintf(outbuf, "%s", "N/A");
        } else {
          if (*pos == 'H') { /* fixed width, 2 digits after the comma */
            outbuf += sprintf(outbuf, "%6.2lf", lasthum);
          } else if (*pos == 'h') { /* variable width, 2 digits after the comma. */
            outbuf += sprintf(outbuf, "%.2lf", lasthum);
          } else if (*pos == 'F') { /* fixed width, 1 digit after the comma. */
            outbuf += sprintf(outbuf, "%5.1lf", lasthum);
          } else if (*pos == 'f') { /* variable width, 1 digit after the comma. */
            outbuf += sprintf(outbuf, "%.1lf", lasthum);
          }
        }
      } else if (*pos == 'C') { /* CO2 */
        if (lastco2 >= 65533) { /* No data yet */
          outbuf += sprintf(outbuf, "%s", "N/A");
        } else {
          outbuf += sprintf(outbuf, "%lu", lastco2);
        }
      } else if (*pos == 'r') { /* carriage return */
        *outbuf = '\r';
        outbuf++;
      } else if (*pos == 'n') { /* linefeed / Newline */
        *outbuf = '\n';
        outbuf++;
      } else if (*pos == '%') { /* literal percent sign */
        *outbuf = '%';
        outbuf++;
      } else if (*pos == 0) {
        *outbuf = 0;
        return;
      }
      pos++;
    } else {
      *outbuf = *pos;
      outbuf++;
      pos++;
    }
  }
  *outbuf = 0;
}

static void dotryrestart(struct daemondata * dd, char ** argv) {
  struct daemondata * curdd = dd;

  if (!restartonerror) {
    exit(1);
  }
  /* close all open sockets */
  while (curdd != NULL) {
    close(curdd->fd);
    curdd = curdd->next;
  }
  fprintf(stderr, "Will try to restart in %d second(s)...\n", restartonerror);
  sleep(restartonerror);
  execv(argv[0], argv);
  exit(1); /* This should of course never be reached */
}

/* This decrypts the 8 byte buffer crypted with the weird CO2 sensor
 * pseudocrypto (what is it good for?)
 * Notes: buf has to be exactly 8 bytes, and simply gets modified
 * by this function. */
void decrypt_8byte_buf(uint8_t * buf) {
  uint8_t cstate[8]  = { 0x48, 0x74, 0x65, 0x6D, 0x70, 0x39, 0x39, 0x65 }; /* "Htemp99e" */
  uint8_t shuffle[8] = { 2, 4, 0, 7, 1, 6, 5, 3 };
  uint8_t phase1[8]  = { 0, 0, 0, 0, 0, 0, 0, 0 };
  uint8_t phase2[8]  = { 0, 0, 0, 0, 0, 0, 0, 0 };
  uint8_t phase3[8]  = { 0, 0, 0, 0, 0, 0, 0, 0 };
  uint8_t ctmp[8]    = { 0, 0, 0, 0, 0, 0, 0, 0 };
  int i;
  
  for (i = 0; i < 8; i++) { /* phase1: shuffle */
    phase1[shuffle[i]] = buf[i];
  }
  for (i = 0; i < 8; i++) { /* phase2: XOR */
    phase2[i] = phase1[i] ^ keyforpseudocrypto[i];
  }
  for (i = 0; i < 8; i++) { /* phase3: shift */
    phase3[i] = ( (phase2[i] >> 3) | (phase2[ (i + 7) % 8 ] << 5) );
  }
  for (i = 0; i < 8; i++) {
    ctmp[i] = ( (cstate[i] >> 4) | (cstate[i] << 4) );
  }
  for (i=0; i<8; i++){
    buf[i] = (0x100 + phase3[i] - ctmp[i]);
  }
}

static int processhidrawdata(int hidrawfd, struct daemondata * dd, char ** argv) {
  unsigned char buf[8];
  int ret; int i;

  ret = read(hidrawfd, buf, sizeof(buf));
  if (ret < 0) {
    fprintf(stderr, "unexpected ERROR reading hidraw input: %s\n", strerror(errno));
    dotryrestart(dd, argv);
  }
  if (ret != 8) {
    return 0;
  }
  decrypt_8byte_buf(buf);
  if (verblev > 3) {
    printf("Received something from the device:");
    for (i = 0; i < ret; i++) {
      printf(" %02x", buf[i]);
    }
    printf("\n");
  }
  /* Now in the 8 bytes received,
   * byte 0 tells you what type of data you just got,
   * byte 1 + 2 tell you the value,
   * byte 3 is a checksum, just bytes 0-2 added together.
   * byte 4 is always 0x0d
   * bytes 5-7 are 0x00. */
  uint16_t valrcvd = (buf[1] << 8) | buf[2];
  if (buf[4] != 0x0d) { return 0; }
  uint8_t checksum = buf[0] + buf[1] + buf[2];
  if (checksum != buf[3]) { return 0; }
  /* OK, this looks valid so far. What is it? */
  if        (buf[0] == 0x50) { /* CO2 */
    lastco2 = valrcvd;
    VERBPRINT(1, "Received CO2-data: CO2=%lu PPM\n", lastco2);
  } else if (buf[0] == 0x42) { /* Temperature */
    lasttemp = ((double)valrcvd / 16.0) - 273.15;
    VERBPRINT(1, "Received Temp-data: Temp=%.2lf degrees\n", lasttemp);
  } else if (buf[0] == 0x44) { /* Humidity */
    /* This is untested, my device does not support it */
    lasthum = (double)valrcvd / 100.0;
    VERBPRINT(1, "Received Humidity-data: Hum=%.0lf%%\n", lasthum);
  }
  return ret;
}

static void dodaemon(int hidrawfd, struct daemondata * dd, char ** argv) {
  fd_set mylsocks;
  struct daemondata * curdd;
  struct timeval to;
  int maxfd;
  int readysocks;
  time_t lastdatarecv;

  lastdatarecv = time(NULL);
  while (1) {
    curdd = dd; /* Start from beginning */
    maxfd = 0;
    FD_ZERO(&mylsocks);
    while (curdd != NULL) {
      FD_SET(curdd->fd, &mylsocks);
      if (curdd->fd > maxfd) { maxfd = curdd->fd; }
      curdd = curdd->next;
    }
    FD_SET(hidrawfd, &mylsocks);
    if (hidrawfd > maxfd) { maxfd = hidrawfd; }
    to.tv_sec = 60; to.tv_usec = 1;
    if ((readysocks = select((maxfd + 1), &mylsocks, NULL, NULL, &to)) < 0) { /* Error?! */
      if (errno != EINTR) {
        perror("ERROR: error on select()");
        dotryrestart(dd, argv);
      }
    } else {
      if (FD_ISSET(hidrawfd, &mylsocks)) {
        if (processhidrawdata(hidrawfd, dd, argv) > 0) {
          lastdatarecv = time(NULL);
        }
      }
      curdd = dd;
      while (curdd != NULL) {
        if (FD_ISSET(curdd->fd, &mylsocks)) {
          int tmpfd;
          struct sockaddr_in6 srcad;
          socklen_t adrlen = sizeof(srcad);
          tmpfd = accept(curdd->fd, (struct sockaddr *)&srcad, &adrlen);
          if (tmpfd < 0) {
            perror("WARNING: Failed to accept() connection");
          } else {
            char outbuf[250];
            printtooutbuf(outbuf, strlen(outbuf), curdd);
            logaccess((struct sockaddr *)&srcad, adrlen, outbuf);
            write(tmpfd, outbuf, strlen(outbuf));
            close(tmpfd);
          }
        }
        curdd = curdd->next;
      }
    }
    if (restartonerror) {
      /* Did we receive something from the hidraw device/socket recently? */
      if ((time(NULL) - lastdatarecv) > 60) {
        fprintf(stderr, "%s\n", "Timeout: No data from serial port for 60 seconds.");
        dotryrestart(dd, argv);
      }
    }
  }
  /* never reached */
}


int main(int argc, char ** argv)
{
  int curarg;
  int hidrawfd;

  for (curarg = 1; curarg < argc; curarg++) {
    if        (strcmp(argv[curarg], "-v") == 0) {
      verblev++;
    } else if (strcmp(argv[curarg], "-q") == 0) {
      verblev--;
    } else if (strcmp(argv[curarg], "-f") == 0) {
      runinforeground = 1;
    } else if (strcmp(argv[curarg], "-h") == 0) {
      usage(argv[0]); exit(0);
    } else if (strcmp(argv[curarg], "--help") == 0) {
      usage(argv[0]); exit(0);
    } else if (strcmp(argv[curarg], "--restartonerror") == 0) {
      restartonerror += 5;
    } else if (strcmp(argv[curarg], "-d") == 0) {
      curarg++;
      if (curarg >= argc) {
        fprintf(stderr, "ERROR: -d requires a parameter!\n");
        usage(argv[0]); exit(1);
      }
      hidrawdevicepath = strdup(argv[curarg]);
    } else {
      /* Unknown - must be the command. */
      break;
    }
  }
  if (curarg == argc) {
    fprintf(stderr, "ERROR: No command given!\n");
    usage(argv[0]);
    exit(1);
  }
  hidrawfd = open(hidrawdevicepath, O_NOCTTY | O_NONBLOCK | O_RDWR);
  if (hidrawfd < 0) {
    fprintf(stderr, "ERROR: Could not open hidraw device %s (%s).\n", hidrawdevicepath, strerror(errno));
    exit(1);
  }
  if (strcmp(argv[curarg], "daemon") == 0) { /* Daemon mode */
    struct daemondata * mydaemondata = NULL;
    curarg++;
    do {
      int l; int optval;
      struct daemondata * newdd;
      struct sockaddr_in6 soa;

      if (curarg >= argc) continue;
      newdd = calloc(sizeof(struct daemondata), 1);
      newdd->next = mydaemondata;
      mydaemondata = newdd;
      l = sscanf(argv[curarg], "%u:%999[^\n]",
                 &mydaemondata->port, &mydaemondata->outputformat[0]);
      if (l < 1) {
        fprintf(stderr, "ERROR: failed to parse daemon command parameter '%s'\n", argv[curarg]);
        exit(1);
      }
      if (l == 1) {
        strcpy((char *)&mydaemondata->outputformat[0], "%C %T");
      }
      /* Open the port */
      mydaemondata->fd = socket(PF_INET6, SOCK_STREAM, 0);
      soa.sin6_family = AF_INET6;
      soa.sin6_addr = in6addr_any;
      soa.sin6_port = htons(mydaemondata->port);
      optval = 1;
      if (setsockopt(mydaemondata->fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) {
        VERBPRINT(0, "WARNING: failed to setsockopt REUSEADDR: %s", strerror(errno));
      }
#ifdef BRAINDEADOS
      /* For braindead operating systems in default config (BSD, Windows,
       * newer Debian), we need to tell the OS that we're actually fine with
       * accepting V4 mapped addresses as well. Because apparently for
       * braindead idiots accepting only selected addresses is a more default
       * case than accepting everything. */
      optval = 0;
      if (setsockopt(mydaemondata->fd, IPPROTO_IPV6, IPV6_V6ONLY, &optval, sizeof(optval))) {
        VERBPRINT(0, "WARNING: failed to setsockopt IPV6_V6ONLY: %s", strerror(errno));
      }
#endif
      if (bind(mydaemondata->fd, (struct sockaddr *)&soa, sizeof(soa)) < 0) {
        perror("Bind failed");
        printf("Could not bind to port %u\n", mydaemondata->port);
        exit(1);
      }
      if (listen(mydaemondata->fd, 20) < 0) { /* Large Queue as we might block for some time while reading */
        perror("Listen failed");
        exit(1);
      }
      curarg++;
    } while (curarg < argc);
    if (mydaemondata == NULL) {
      fprintf(stderr, "ERROR: the daemon command requires parameters.\n");
      exit(1);
    }
    {
      /* Init the CO2 sensor. */
      unsigned char ioctlparams[9];
      int k;
      /* What we send with the ioctl is just the key prefixed with 0x00. */
      ioctlparams[0] = 0;
      for (k = 0; k < 8; k++) {
        ioctlparams[k+1] = keyforpseudocrypto[k];
      }
      ioctl(hidrawfd, HIDIOCSFEATURE(9), ioctlparams);
    }
    /* the good old doublefork trick from 'systemprogrammierung 1' */
    if (runinforeground != 1) {
      int ourpid;
      VERBPRINT(2, "launching into the background...\n");
      ourpid = fork();
      if (ourpid < 0) {
        perror("Ooops, fork() #1 failed");
        exit(1);
      }
      if (ourpid == 0) { /* We're the child */
        ourpid = fork(); /* fork again */
        if (ourpid < 0) {
          perror("Ooooups. fork() #2 failed");
          exit(1);
        }
        if (ourpid == 0) { /* Child again */
          /* Just don't exit, we'll continue below. */
        } else { /* Parent */
          exit(0); /* Just exit */
        }
      } else { /* Parent */
        exit(0); /* Just exit */
      }
    }
    {
      struct sigaction sia;
      sia.sa_handler = sigpipehandler;
      sigemptyset(&sia.sa_mask); /* If we don't do this, we're likely */
      sia.sa_flags = 0;          /* to die from 'broken pipe'! */
      sigaction(SIGPIPE, &sia, NULL);
    }
    dodaemon(hidrawfd, mydaemondata, argv);
  } else {
    fprintf(stderr, "ERROR: Command '%s' is unknown.\n", argv[curarg]);
    usage(argv[0]);
    exit(1);
  }
  return 0;
}
