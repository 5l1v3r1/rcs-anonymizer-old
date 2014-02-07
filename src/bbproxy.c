#include "bbproxy.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <syslog.h>
#include <features.h>

#define LOGQUEUE_SIZE 500

int main(int argc, char *argv[]);
int readconfig(char *filename, char *param, int len);
void sighandler(int s);
extern int managerhandler(BIO *cbio);
extern int proxyhandler(BIO *cbio);

int ifd[2], lfd[2];

int main(int argc, char *argv[])
{
   BIO *pbio = NULL, *mbio = NULL, *cbio = NULL;
   int pfd, mfd, cfd, sfd, ret;
   fd_set rfds;
   char *bp;

   struct log l, ls, lq[LOGQUEUE_SIZE], *lp, *lh;
   int cmd;

   openlog(PROGNAME, LOG_PID, LOG_DAEMON);

   conf.loglevel = LOGMSG_INFO;
   if((argc > 1) && !strcmp(argv[1], "-d")) conf.loglevel = LOGMSG_DEBUG;
   memset(lq, 0x00, sizeof(lq));
   lp = lq;
   pipe(lfd);

   socketpair(AF_UNIX, SOCK_DGRAM, 0, ifd);

   readconfig_str(VERSION_FILE, &conf.version);
   if(!conf.version) conf.version = strdup(VERSION_DEFAULT);
   logme(LOGMSG_INFO, "Starting daemon (version %s)", conf.version);

   readconfig_str(CERTIFICATE_FILE, &bp);
   if(!bp) {
      logme(LOGMSG_ERROR, "Unable to read certificate file");
      exit(EXIT_FAILURE);
   }
   free(bp);
   logme(LOGMSG_DEBUG, "Certificate file is " CERTIFICATE_FILE);

   readconfig_str(SIGNATURE_FILE, &conf.signature);
   if(!conf.signature) {
      logme(LOGMSG_ERROR, "Unable to read signature");
      exit(EXIT_FAILURE);
   }
   logme(LOGMSG_DEBUG, "Signature is %s", conf.signature);

   readconfig_str(MANAGERPORT_FILE, &conf.managerport);
   if(!conf.managerport || (atoi(conf.managerport) <= 0) || (atoi(conf.managerport) > 65535)) {
      if(conf.managerport) free(conf.managerport);
      conf.managerport = strdup(MANAGERPORT_DEFAULT);
   }
   mbio = BIO_new_accept(conf.managerport);
   BIO_set_bind_mode(mbio, BIO_BIND_REUSEADDR);
   if(BIO_do_accept(mbio) <= 0) {
      logme(LOGMSG_ERROR, "Unable to bind manager port (%s)", conf.managerport);
      exit(EXIT_FAILURE);
   }
   logme(LOGMSG_DEBUG, "Listening to manager port (%s)", conf.managerport);
   mfd = BIO_get_fd(mbio, NULL);

   readconfig_str(PROXYPORT_FILE, &conf.proxyport);
   if(!conf.proxyport || (atoi(conf.proxyport) <= 0) || (atoi(conf.proxyport) > 65535)) {
      if(conf.proxyport) free(conf.proxyport);
      conf.proxyport = strdup(PROXYPORT_DEFAULT);
   }
   pbio = BIO_new_accept(conf.proxyport);
   BIO_set_bind_mode(pbio, BIO_BIND_REUSEADDR);
   if(BIO_do_accept(pbio) <= 0) {
      logme(LOGMSG_ERROR, "Unable to bind proxy port (%s)", conf.proxyport);
      exit(EXIT_FAILURE);
   }
   logme(LOGMSG_DEBUG, "Listening to proxy port (%s)", conf.proxyport);
   pfd = BIO_get_fd(pbio, NULL);

   readconfig_str(NEXTHOP_FILE, &conf.nexthop);
   if(!conf.nexthop) logme(LOGMSG_STATUSERROR, "Link not configured, proxy disabled");
   else if(conf.nexthop[0] == '-') logme(LOGMSG_INFO, "Link not in chain, proxy disabled");
   else logme(LOGMSG_INFO, "Nexthop is %s", conf.nexthop);

   ls.level = -1;
   if(conf.nexthop) logme(LOGMSG_STATUSOK, "Running");

   signal(SIGHUP, sighandler);
   signal(SIGCHLD, sighandler);

   logme(LOGMSG_INFO, "Daemon started");

   sfd = mfd;
   if(pfd > sfd) sfd = pfd;
   if(lfd[0] > sfd) sfd = lfd[0];
   if(ifd[0] > sfd) sfd = ifd[0];

   while(1) {
      FD_ZERO(&rfds);
      FD_SET(mfd, &rfds);
      FD_SET(pfd, &rfds);
      FD_SET(lfd[0], &rfds);
      FD_SET(ifd[0], &rfds);
      if(select(sfd + 1, &rfds, NULL, NULL, NULL) == -1) continue;

      if(FD_ISSET(mfd, &rfds)) {
         logme(LOGMSG_DEBUG, "Incoming manager connection");
         if(BIO_do_accept(mbio) <= 0) {
            logme(LOGMSG_ERROR, "Unable to handle manager connection");
            continue;
         }
         cbio = BIO_pop(mbio);
         cfd = BIO_get_fd(cbio, NULL);

         if(!fork()) {
            signal(SIGHUP, SIG_IGN);
            signal(SIGCHLD, SIG_DFL);
            close(lfd[0]);
            close(mfd);
            close(pfd);
            ret = managerhandler(cbio);
            if(ret < 0) {
               logme(LOGMSG_ERROR, "Manager handler exited with errors");
            } else {
               logme(LOGMSG_DEBUG, "Manager handler exited with no errors");
            }
            return ret;
         }

         close(cfd);
         BIO_free(cbio);
      }

      if(FD_ISSET(pfd, &rfds)) {
         logme(LOGMSG_DEBUG, "Incoming proxy connection");
         if(BIO_do_accept(pbio) <= 0) {
            logme(LOGMSG_ERROR, "Unable to handle proxy connection");
            continue;
         }
         cbio = BIO_pop(pbio);
         cfd = BIO_get_fd(cbio, NULL);

         if(conf.nexthop && (conf.nexthop[0] != '-')) {
            if(!fork()) {
               signal(SIGHUP, SIG_IGN);
               signal(SIGCHLD, SIG_DFL);
               close(lfd[0]);
               close(ifd[0]);
               close(ifd[1]);
               close(mfd);
               close(pfd);
               ret = proxyhandler(cbio);
               if(ret < 0) {
                  logme(LOGMSG_ERROR, "Proxy handler exited with errors");
               } else {
                  logme(LOGMSG_DEBUG, "Proxy handler exited with no errors");
               }
               return ret;
            }
         } else {
            logme(LOGMSG_DEBUG, "Proxy disabled, closing connection");
         }

         close(cfd);
         BIO_free(cbio);
      }

      if(FD_ISSET(lfd[0], &rfds)) {
         read(lfd[0], &l, sizeof(l));

         switch(l.level) {
            case LOGMSG_STATUSOK:
               if(ls.level == LOGMSG_STATUSOK) {
                  logme(LOGMSG_DEBUG, l.data);
               } else {
                  logme(LOGMSG_INFO, l.data);
               }
               memcpy(&ls, &l, sizeof(l));
               break;
            case LOGMSG_STATUSERROR:
               logme(LOGMSG_ERROR, l.data);
               memcpy(&ls, &l, sizeof(l));
               break;
            default:
               memcpy(lp, &l, sizeof(l));
               lp = (lp < (lq + LOGQUEUE_SIZE - 1)) ? lp + 1 : lq;
               break;
         }
      }

      if(FD_ISSET(ifd[0], &rfds)) {
         read(ifd[0], &cmd, sizeof(cmd));

         switch(cmd) {
            case COMMAND_GETSTATUS:
               write(ifd[0], &ls, sizeof(ls));
               break;
            case COMMAND_GETLOG:
               lh = lp;
               do {
                  if(lh->level != LOGMSG_EMPTY) write(ifd[0], lh, sizeof(lq[0]));
                  lh = (lh < (lq + LOGQUEUE_SIZE - 1)) ? lh + 1 : lq;
               } while(lh != lp);
               memset(lq, 0x00, sizeof(lq));
               lp = lq;
               write(ifd[0], lp, sizeof(lq[0]));
               break;
         }
      }
   }

   return 0;
}

int readconfig(char *filename, char *param, int len)
{
   FILE *fp;

   if(len <= 0) return -1;

   if((fp = fopen(filename, "r")) == NULL) return -1;
   if(fgets(param, len, fp) == NULL) {
      param[0] = '\0';
   } else {
      len = strlen(param);
      while((len > 0) && isspace(param[len - 1])) len--;
      param[len] = '\0';
   }
   fclose(fp);

   return strlen(param);
}

int readconfig_str(char *filename, char **param)
{
   char buf[1024];

   *param = NULL;
   if(readconfig(filename, buf, sizeof(buf)) == -1) return -1;
   *param = strdup(buf);

   return 0;
}

int readconfig_int(char *filename, int *param)
{
   char buf[11];

   *param = 0;
   if(readconfig(filename, buf, sizeof(buf)) == -1) return -1;
   *param = atoi(buf);

   return 0;
}

void logme(int level, char *format, ...)
{
   va_list ap;
   struct log l;
   char *prefix = NULL;
   int sl;

   if(level > conf.loglevel) return;

   va_start(ap, format);

   switch(level) {
      case LOGMSG_INFO:
         prefix = "INFO";
         sl = LOG_INFO;
         break;
      case LOGMSG_ERROR:
         prefix = "ERROR";
         sl = LOG_ERR;
         break;
      case LOGMSG_DEBUG:
         prefix = "DEBUG";
         sl = LOG_DEBUG;
         break;
      case LOGMSG_STATUSOK:
         prefix = "STATUSOK";
         sl = -1;
         break;
      case LOGMSG_STATUSERROR:
         prefix = "STATUSERROR";
         sl = -1;
         break;
   }

   fprintf(stderr, "[%s] ", prefix);
   vfprintf(stderr, format, ap);
   fprintf(stderr, "\n");

   if(sl != -1) vsyslog(LOG_DAEMON|sl, format, ap);

   l.level = level;
   l.ts = time(NULL);
   vsnprintf(l.data, sizeof(l.data), format, ap);
   write(lfd[1], &l, sizeof(l));

   va_end(ap);

   return;
}

void sighandler(int s)
{
   if(s == SIGHUP) {
      logme(LOGMSG_DEBUG, "SIGHUP received");

      if(conf.nexthop) free(conf.nexthop);
      readconfig_str(NEXTHOP_FILE, &conf.nexthop);
      if(!conf.nexthop) logme(LOGMSG_ERROR, "Link not configured, proxy disabled");
      else if(conf.nexthop[0] == '-') logme(LOGMSG_INFO, "Link not in chain, proxy disabled");
      else logme(LOGMSG_INFO, "Nexthop is %s", conf.nexthop);

      if(conf.nexthop) logme(LOGMSG_STATUSOK, "Running");
      else logme(LOGMSG_STATUSERROR, "Link not configured, proxy disabled");
   } else if(s == SIGCHLD) {
      wait(NULL);
   }

   signal(s, sighandler);

   return;
}
