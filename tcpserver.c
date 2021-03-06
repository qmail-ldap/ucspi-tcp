#include <sys/types.h>
#include <sys/param.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include "uint16.h"
#include "str.h"
#include "byte.h"
#include "fmt.h"
#include "scan.h"
#include "ip4.h"
#include "fd.h"
#include "exit.h"
#include "env.h"
#include "prot.h"
#include "open.h"
#include "wait.h"
#include "readwrite.h"
#include "stralloc.h"
#include "alloc.h"
#include "buffer.h"
#include "error.h"
#include "strerr.h"
#include "sgetopt.h"
#include "pathexec.h"
#include "socket.h"
#include "ndelay.h"
#include "remoteinfo.h"
#include "rules.h"
#include "sig.h"
#include "dns.h"

int verbosity = 1;
int flagkillopts = 1;
int flagdelay = 1;
char *banner = "";
int flagremoteinfo = 1;
int flagremotehost = 1;
int flagparanoid = 0;
unsigned long timeout = 26;
#ifdef WITH_SSL
int flagssl = 0;
struct stralloc certfile = {0};
#define CERTFILE "./cert.pem"

void translate(SSL*, int, int, unsigned int);
#endif

static stralloc tcpremoteinfo;

uint16 localport;
char localportstr[FMT_ULONG];
char localip[4];
char localipstr[IP4_FMT];
static stralloc localhostsa;
char *localhost = 0;

uint16 remoteport;
char remoteportstr[FMT_ULONG];
char remoteip[4];
char remoteipstr[IP4_FMT];
static stralloc remotehostsa;
char *remotehost = 0;

char strnum[FMT_ULONG];
char strnum2[FMT_ULONG];

static stralloc tmp;
static stralloc fqdn;
static stralloc addresses;

char bspace[16];
buffer b;



/* ---------------------------- child */

#define DROP "tcpserver: warning: dropping connection, "

int flagdeny = 0;
int flagallownorules = 0;
char *fnrules = 0;

void drop_nomem(void)
{
  strerr_die2sys(111,DROP,"out of memory");
}
void cats(char *s)
{
  if (!stralloc_cats(&tmp,s)) drop_nomem();
}
void append(char *ch)
{
  if (!stralloc_append(&tmp,ch)) drop_nomem();
}
void safecats(char *s)
{
  char ch;
  int i;

  for (i = 0;i < 100;++i) {
    ch = s[i];
    if (!ch) return;
    if (ch < 33) ch = '?';
    if (ch > 126) ch = '?';
    if (ch == '%') ch = '?'; /* logger stupidity */
    if (ch == ':') ch = '?';
    append(&ch);
  }
  cats("...");
}
void env(char *s,char *t)
{
  if (!pathexec_env(s,t)) drop_nomem();
}
void drop_rules(void)
{
  strerr_die4sys(111,DROP,"unable to read ",fnrules,": ");
}

void found(char *data,unsigned int datalen)
{
  unsigned int next0;
  unsigned int split;

  while ((next0 = byte_chr(data,datalen,0)) < datalen) {
    switch(data[0]) {
      case 'D':
	flagdeny = 1;
	break;
      case '+':
	split = str_chr(data + 1,'=');
	if (data[1 + split] == '=') {
	  data[1 + split] = 0;
	  env(data + 1,data + 1 + split + 1);
	}
	break;
      case '-':
	env(data + 1, (char *)0);
	break;
    }
    ++next0;
    data += next0; datalen -= next0;
  }
}

void doit(int t)
{
  int j;

  remoteipstr[ip4_fmt(remoteipstr,remoteip)] = 0;

  if (verbosity >= 2) {
    strnum[fmt_ulong(strnum,getpid())] = 0;
    strerr_warn4("tcpserver: pid ",strnum," from ",remoteipstr,0);
  }

  if (flagkillopts)
    socket_ipoptionskill(t);
  if (!flagdelay)
    socket_tcpnodelay(t);

  if (*banner) {
    buffer_init(&b,write,t,bspace,sizeof bspace);
    if (buffer_putsflush(&b,banner) == -1)
      strerr_die2sys(111,DROP,"unable to print banner: ");
  }

  if (socket_local4(t,localip,&localport) == -1)
    strerr_die2sys(111,DROP,"unable to get local address: ");

  localipstr[ip4_fmt(localipstr,localip)] = 0;
  remoteportstr[fmt_ulong(remoteportstr,remoteport)] = 0;

  if (!localhost)
    if (dns_name4(&localhostsa,localip) == 0)
      if (localhostsa.len) {
	if (!stralloc_0(&localhostsa)) drop_nomem();
	localhost = localhostsa.s;
      }
  env("PROTO","TCP");
  env("TCPLOCALIP",localipstr);
  env("TCPLOCALPORT",localportstr);
  env("TCPLOCALHOST",localhost);

  if (flagremotehost)
    if (dns_name4(&remotehostsa,remoteip) == 0)
      if (remotehostsa.len) {
	if (flagparanoid)
	  if (dns_ip4(&tmp,&remotehostsa) == 0)
	    for (j = 0;j + 4 <= tmp.len;j += 4)
	      if (byte_equal(remoteip,4,tmp.s + j)) {
		flagparanoid = 0;
		break;
	      }
	if (!flagparanoid) {
	  if (!stralloc_0(&remotehostsa)) drop_nomem();
	  remotehost = remotehostsa.s;
	}
      }
  env("TCPREMOTEIP",remoteipstr);
  env("TCPREMOTEPORT",remoteportstr);
  env("TCPREMOTEHOST",remotehost);

  if (flagremoteinfo) {
    if (remoteinfo(&tcpremoteinfo,remoteip,remoteport,localip,localport,timeout) == -1)
      flagremoteinfo = 0;
    if (!stralloc_0(&tcpremoteinfo)) drop_nomem();
  }
  env("TCPREMOTEINFO",flagremoteinfo ? tcpremoteinfo.s : 0);

  if (fnrules) {
    int fdrules;
    fdrules = open_read(fnrules);
    if (fdrules == -1) {
      if (errno != error_noent) drop_rules();
      if (!flagallownorules) drop_rules();
    }
    else {
      if (rules(found,fdrules,remoteipstr,remotehost,flagremoteinfo ? tcpremoteinfo.s : 0) == -1) drop_rules();
      close(fdrules);
    }
  }

  if (verbosity >= 2) {
    strnum[fmt_ulong(strnum,getpid())] = 0;
    if (!stralloc_copys(&tmp,"tcpserver: ")) drop_nomem();
    safecats(flagdeny ? "deny" : "ok");
    cats(" "); safecats(strnum);
    cats(" "); if (localhost) safecats(localhost);
    cats(":"); safecats(localipstr);
    cats(":"); safecats(localportstr);
    cats(" "); if (remotehost) safecats(remotehost);
    cats(":"); safecats(remoteipstr);
    cats(":"); if (flagremoteinfo) safecats(tcpremoteinfo.s);
    cats(":"); safecats(remoteportstr);
    cats("\n");
    buffer_putflush(buffer_2,tmp.s,tmp.len);
  }

  if (flagdeny) _exit(100);
}



/* ---------------------------- parent */

#define FATAL "tcpserver: fatal: "

void usage(void)
{
#ifndef WITH_SSL
  strerr_warn1("\
tcpserver: usage: tcpserver \
[ -1UXpPhHrRoOdDqQv ] \
[ -c limit ] \
[ -C [address[/len]:]limit ] \
[ -e name=var ] \
[ -x rules.cdb ] \
[ -B banner ] \
[ -g gid ] \
[ -u uid ] \
[ -b backlog ] \
[ -l localname ] \
[ -t timeout ] \
host port program",0);
#else
  strerr_warn1("\
tcpserver: usage: tcpserver \
[ -1UXpPhHrRoOdDqQsSv ] \
[ -c limit ] \
[ -C [address[/len]:]limit ] \
[ -e name=var ] \
[ -x rules.cdb ] \
[ -B banner ] \
[ -g gid ] \
[ -u uid ] \
[ -b backlog ] \
[ -l localname ] \
[ -t timeout ] \
[ -n certfile ] \
host port program",0);
#endif
  _exit(100);
}

unsigned long limit = 40;
unsigned long numchildren = 0;

int flag1 = 0;
unsigned long backlog = 20;
unsigned long uid = 0;
unsigned long gid = 0;

void printstatus(void)
{
  if (verbosity < 2) return;
  strnum[fmt_ulong(strnum,numchildren)] = 0;
  strnum2[fmt_ulong(strnum2,limit)] = 0;
  strerr_warn4("tcpserver: status: ",strnum,"/",strnum2,0);
}

void sigterm()
{
  _exit(0);
}

struct conn {
  int pid;
  char remoteip[4];
} *conns;

struct ip_limelt {
  char ip[4];
  char mask[4];
  unsigned long limit;
  unsigned long count;
};

#include "gen_alloc.h"
#include "gen_allocdefs.h"
GEN_ALLOC_typedef(ip_limit,struct ip_limelt,l,len,a)
GEN_ALLOC_readyplus(ip_limit, struct ip_limelt,l,len,a,i,n,x,10,
  ip_limit_rp)

ip_limit ipl = {0};
unsigned long limit_ip = 0;

void
ip_limit_add(char *str)
{
  unsigned int n, len;
  unsigned long ul = 0;
  struct ip_limelt lim;

  byte_zero(&lim, sizeof(lim));
  n = str_chr(str, ':');
  if (str[n] == ':') {
    str[n] = 0;
    scan_ulong(str + n + 1, &ul);
    lim.limit = ul;
    /* parse ip */
    ul = 32;
    n = str_chr(str, '/');
    if (str[n] == '/')
      scan_ulong(str + n + 1, &ul);
    if (ul > 32)
      strerr_die2x(111,FATAL,"ip prefix len > 32");
    if (ip4_scan(str, lim.ip) == 0)
      strerr_die2x(111,FATAL,"bad ip address");
    for (n = 0; n < 4; n++)
      if (ul > 8) {
	lim.mask[n] = 0xff;
	ul -= 8;
      } else {
	lim.mask[n] = 0xff << (8 - ul);
	ul = 0;
      }
    if (!ip_limit_rp(&ipl,1))
      strerr_die2x(111,FATAL,"out of memory");
    ipl.l[ipl.len++] = lim;
  } else {
    scan_ulong(str, &ul);
    limit_ip = ul;
  }
}

int
ip_limit_check(char ip[4], int d)
{
  unsigned long c;
  unsigned int l;
  int i;

  for (l = 0; l < ipl.len; l++)
    if ((ip[0] & ipl.l[l].mask[0]) == (ipl.l[l].ip[0] & ipl.l[l].mask[0]) &&
        (ip[1] & ipl.l[l].mask[1]) == (ipl.l[l].ip[1] & ipl.l[l].mask[1]) &&
        (ip[2] & ipl.l[l].mask[2]) == (ipl.l[l].ip[2] & ipl.l[l].mask[2]) &&
        (ip[3] & ipl.l[l].mask[3]) == (ipl.l[l].ip[3] & ipl.l[l].mask[3])) {
      if (ipl.l[l].count + d > ipl.l[l].limit)
	return 1;
      ipl.l[l].count += d;
      return 0;
    }

  /* global per ip limit */
  for (l = 0, c= 0; l < limit; l++)
    if (!byte_diff(conns[l].remoteip, sizeof(ip), ip))
      c++;
  if (limit_ip != 0 && c + d > limit_ip)
    return 1;

  return 0;
}

void sigchld()
{
  int wstat;
  int pid;
  unsigned int i;

  while ((pid = wait_nohang(&wstat)) > 0) {
    for (i = 0; i < limit; i++)
      if (conns[i].pid == pid) {
	ip_limit_check(conns[i].remoteip, -1);
	byte_zero(&conns[i], sizeof(struct conn));
      }
    if (verbosity >= 2) {
      strnum[fmt_ulong(strnum,pid)] = 0;
      strnum2[fmt_ulong(strnum2,wstat)] = 0;
      strerr_warn4("tcpserver: end ",strnum," status ",strnum2,0);
    }
    if (numchildren) --numchildren; printstatus();
  }
}

main(int argc,char **argv)
{
  char *hostname;
  char *portname;
  int opt;
  struct servent *se;
  char *x;
  char *iplimenv;
  int flagiplim;
  unsigned long u;
  int s;
  int t;
  unsigned int i;
  int pid;
#ifdef WITH_SSL
  BIO *sbio;
  SSL *ssl;
  SSL_CTX *ctx;
  int pi2c[2], pi4c[2];

  ctx = NULL;

  if (!stralloc_copys(&certfile, CERTFILE) || !stralloc_0(&certfile) )
    strerr_die2x(111,FATAL,"out of memory");
  while ((opt = getopt(argc,argv,"dDvqQhHrRsS1UXx:t:u:g:l:b:B:c:C:e:n:pPoO")) != opteof)
#else
  while ((opt = getopt(argc,argv,"dDvqQhHrR1UXx:t:u:g:l:b:B:c:C:e:pPoO")) != opteof)
#endif
    switch(opt) {
      case 'b': scan_ulong(optarg,&backlog); break;
      case 'c': scan_ulong(optarg,&limit); break;
      case 'C': ip_limit_add(optarg); break;
      case 'X': flagallownorules = 1; break;
      case 'x': fnrules = optarg; break;
      case 'B': banner = optarg; break;
      case 'd': flagdelay = 1; break;
      case 'D': flagdelay = 0; break;
      case 'v': verbosity = 2; break;
      case 'q': verbosity = 0; break;
      case 'Q': verbosity = 1; break;
      case 'P': flagparanoid = 0; break;
      case 'p': flagparanoid = 1; break;
      case 'O': flagkillopts = 1; break;
      case 'o': flagkillopts = 0; break;
      case 'H': flagremotehost = 0; break;
      case 'h': flagremotehost = 1; break;
      case 'R': flagremoteinfo = 0; break;
      case 'r': flagremoteinfo = 1; break;
      case 't': scan_ulong(optarg,&timeout); break;
      case 'U': x = env_get("UID"); if (x) scan_ulong(x,&uid);
		x = env_get("GID"); if (x) scan_ulong(x,&gid); break;
      case 'u': scan_ulong(optarg,&uid); break;
      case 'g': scan_ulong(optarg,&gid); break;
      case '1': flag1 = 1; break;
      case 'l': localhost = optarg; break;
      case 'e': iplimenv = optarg;
		if (iplimenv[str_chr(iplimenv, '=')] != '=')
		  strerr_die2x(100,FATAL, "no '=' in ip limit env-var");
		break;
#ifdef WITH_SSL
      case 's': flagssl = 1; break;
      case 'S': flagssl = 0; break;
      case 'n': if (!stralloc_copys(&certfile, optarg) ||
		    !stralloc_0(&certfile) )
		  strerr_die2x(111,FATAL,"out of memory");
		break;
#endif
      default: usage();
    }
  argc -= optind;
  argv += optind;

  if (!verbosity)
    buffer_2->fd = -1;

  if (limit == 0)
    strerr_die2x(100,FATAL,"limit may not be set to 0");
  if (limit > 65000)
    strerr_die2x(100,FATAL,"limit way to high");
  conns = (struct conn *)alloc(limit * sizeof(struct conn));
  if (!conns)
    strerr_die2x(111,FATAL,"out of memory");
  byte_zero(conns, limit * sizeof(struct conn));
 
  hostname = *argv++;
  if (!hostname) usage();
  if (str_equal(hostname,"")) hostname = "0.0.0.0";
  if (str_equal(hostname,"0")) hostname = "0.0.0.0";

  x = *argv++;
  if (!x) usage();
  if (!x[scan_ulong(x,&u)])
    localport = u;
  else {
    se = getservbyname(x,"tcp");
    if (!se)
      strerr_die3x(111,FATAL,"unable to figure out port number for ",x);
    localport = ntohs(se->s_port);
  }

  if (!*argv) usage();
 
  sig_block(sig_child);
  sig_catch(sig_child,sigchld);
  sig_catch(sig_term,sigterm);
  sig_ignore(sig_pipe);
 
  if (!stralloc_copys(&tmp,hostname))
    strerr_die2x(111,FATAL,"out of memory");
  if (dns_ip4_qualify(&addresses,&fqdn,&tmp) == -1)
    strerr_die4sys(111,FATAL,"temporarily unable to figure out IP address for ",hostname,": ");
  if (addresses.len < 4)
    strerr_die3x(111,FATAL,"no IP address for ",hostname);
  byte_copy(localip,4,addresses.s);

#ifdef WITH_SSL
  if (flagssl == 1) {
    /* setup SSL context (load key and cert into ctx) */
    SSL_library_init();
    ctx=SSL_CTX_new(SSLv23_server_method());
    if (!ctx) strerr_die2x(111,FATAL,"unable to create SSL context");

    /* set prefered ciphers */
    if (env_get("SSL_CIPHER"))
      if (SSL_CTX_set_cipher_list(ctx, env_get("SSL_CIPHER")) == 0)
	strerr_die2x(111,FATAL,"unable to set cipher list");

    if(SSL_CTX_use_RSAPrivateKey_file(ctx, certfile.s, SSL_FILETYPE_PEM) != 1)
      strerr_die2x(111,FATAL,"unable to load RSA private key");
    if(SSL_CTX_use_certificate_chain_file(ctx, certfile.s) != 1)
      strerr_die2x(111,FATAL,"unable to load certificate");
  }
#endif
  
  s = socket_tcp();
  if (s == -1)
    strerr_die2sys(111,FATAL,"unable to create socket: ");
  if (socket_bind4_reuse(s,localip,localport) == -1)
    strerr_die2sys(111,FATAL,"unable to bind: ");
  if (socket_local4(s,localip,&localport) == -1)
    strerr_die2sys(111,FATAL,"unable to get local address: ");
  if (socket_listen(s,backlog) == -1)
    strerr_die2sys(111,FATAL,"unable to listen: ");
  ndelay_off(s);

  if (gid) if (prot_gid(gid) == -1)
    strerr_die2sys(111,FATAL,"unable to set gid: ");
  if (uid) if (prot_uid(uid) == -1)
    strerr_die2sys(111,FATAL,"unable to set uid: ");

 
  localportstr[fmt_ulong(localportstr,localport)] = 0;
  if (flag1) {
    buffer_init(&b,write,1,bspace,sizeof bspace);
    buffer_puts(&b,localportstr);
    buffer_puts(&b,"\n");
    buffer_flush(&b);
  }
 
  close(0);
  close(1);
  printstatus();
 
  for (;;) {
    while (numchildren >= limit) sig_pause();

    sig_unblock(sig_child);
    t = socket_accept4(s,remoteip,&remoteport);
    sig_block(sig_child);

    if (t == -1) continue;
    ++numchildren; printstatus();

    /* per ip handling */
    flagiplim = 0;
    if (ip_limit_check(remoteip, 1)) {
      remoteipstr[ip4_fmt(remoteipstr,remoteip)] = 0;
      if (iplimenv) {
        strerr_warn4(DROP,"too many conections from ",remoteipstr,
	    " only flagged",0);
	flagiplim = 1;
      } else {
        strerr_warn3(DROP,"too many conections from ",remoteipstr,0);
        --numchildren; printstatus();
        close(t);
        continue;
      }
    }
 
    switch(pid = fork()) {
      case 0:
	if (flagiplim) {
          int split;
      	  split = str_chr(iplimenv,'=');
	  iplimenv[split] = 0;
	  env(iplimenv,iplimenv + split + 1);
	}
        close(s);
        doit(t);
        if ((fd_move(0,t) == -1) || (fd_copy(1,0) == -1))
	  strerr_die2sys(111,DROP,"unable to set up descriptors: ");
        sig_uncatch(sig_child);
        sig_unblock(sig_child);
        sig_uncatch(sig_term);
        sig_uncatch(sig_pipe);
#ifdef WITH_SSL
	if (flagssl == 1) {
	  if (pipe(pi2c) != 0)
	    strerr_die2sys(111,DROP,"unable to create pipe: ");
	  if (pipe(pi4c) != 0)
	    strerr_die2sys(111,DROP,"unable to create pipe: ");
	  switch(fork()) {
	    case 0:
	      close(0); close(1);
	      close(pi2c[1]);
	      close(pi4c[0]);
	      if ((fd_move(0,pi2c[0]) == -1) || (fd_move(1,pi4c[1]) == -1))
		strerr_die2sys(111,DROP,"unable to set up descriptors: ");
	      /* signals are allready set in the parent */
	      pathexec(argv);
	      strerr_die4sys(111,DROP,"unable to run ",*argv,": ");
	    case -1:
	      strerr_die2sys(111,DROP,"unable to fork: ");
	    default:
	      ssl = SSL_new(ctx);
	      if (!ssl)
		strerr_die2x(111,DROP,"unable to set up SSL session");
	      sbio = BIO_new_socket(0,BIO_NOCLOSE);
	      if (!sbio)
		strerr_die2x(111,DROP,"unable to set up BIO socket");
	      SSL_set_bio(ssl,sbio,sbio);
	      close(pi2c[0]);
	      close(pi4c[1]);
	      translate(ssl, pi2c[1], pi4c[0], 3600);
	      _exit(0);
	  }
	}
#endif
        pathexec(argv);
	strerr_die4sys(111,DROP,"unable to run ",*argv,": ");
      case -1:
        strerr_warn2(DROP,"unable to fork: ",&strerr_sys);
        --numchildren; printstatus();
	break;
      default:
	for (i = 0; i < limit; i++)
	  if (conns[i].pid == 0) {
	    conns[i].pid = pid;
	    byte_copy(conns[i].remoteip, sizeof(remoteip), remoteip);
	    break;
	  }
    }
    close(t);
  }
}

#ifdef WITH_SSL
static int allwrite(int fd, char *buf, int len)
{
  int w;

  while (len) {
    w = write(fd,buf,len);
    if (w == -1) {
      if (errno == error_intr) continue;
      return -1; /* note that some data may have been written */
    }
    if (w == 0) ; /* luser's fault */
    buf += w;
    len -= w;
  }
  return 0;
}

static int allwritessl(SSL* ssl, char *buf, int len)
{
  int w;

  while (len) {
    w = SSL_write(ssl,buf,len);
    if (w == -1) {
      if (errno == error_intr) continue;
      return -1; /* note that some data may have been written */
    }
    if (w == 0) ; /* luser's fault */
    buf += w;
    len -= w;
  }
  return 0;
}

char tbuf[4096];

void translate(SSL* ssl, int clearout, int clearin, unsigned int iotimeout)
{
  struct taia now;
  struct taia deadline;
  iopause_fd iop[2];
  int flagexitasap;
  int iopl;
  int sslout, sslin;
  int n, r;

  sslin = SSL_get_fd(ssl);
  sslout = SSL_get_fd(ssl);
  if (sslin == -1 || sslout == -1)
    strerr_die2x(111,DROP,"unable to set up SSL connection");
  
  flagexitasap = 0;

  if (SSL_accept(ssl)<=0)
    strerr_die2x(111,DROP,"unable to accept SSL connection");

  while (!flagexitasap) {
    taia_now(&now);
    taia_uint(&deadline,iotimeout);
    taia_add(&deadline,&now,&deadline);

    /* fill iopause struct */
    iopl = 2;
    iop[0].fd = sslin;
    iop[0].events = IOPAUSE_READ;
    iop[1].fd = clearin;
    iop[1].events = IOPAUSE_READ;

    /* do iopause read */
    iopause(iop,iopl,&deadline,&now);
    if (iop[0].revents) {
      /* data on sslin */
      do {
	n = SSL_read(ssl, tbuf, sizeof(tbuf));
	if ( n < 0 )
	  strerr_die2sys(111,DROP,"unable to read form network: ");
	if ( n == 0 )
	  flagexitasap = 1;
	r = allwrite(clearout, tbuf, n);
	if ( r < 0 )
	  strerr_die2sys(111,DROP,"unable to write to client: ");
      } while(!flagexitasap && SSL_pending(ssl));
    }
    if (iop[1].revents) {
      /* data on clearin */
      n = read(clearin, tbuf, sizeof(tbuf));
      if ( n < 0 )
	strerr_die2sys(111,DROP,"unable to read form client: ");
      if ( n == 0 )
	flagexitasap = 1;
      r = allwritessl(ssl, tbuf, n);
      if ( r < 0 )
	strerr_die2sys(111,DROP,"unable to write to network: ");
    }
    if (!iop[0].revents && !iop[1].revents)
      strerr_die2x(0, DROP,"timeout reached without input");
  }
}
#endif
