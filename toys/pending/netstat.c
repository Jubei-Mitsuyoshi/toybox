/* netstat.c - Display Linux networking subsystem.
 *
 * Copyright 2012 Ranjan Kumar <ranjankumar.bth@gmail.com>
 * Copyright 2013 Kyungwan Han <asura321@gmail.com>
 *
 * Not in SUSv4.
 *
USE_NETSTAT(NEWTOY(netstat, "pWrxwutneal", TOYFLAG_BIN))
config NETSTAT
  bool "netstat"
  default n
  help
    usage: netstat [-pWrxwutneal]

    Display networking information.

    -r  routing table
    -a  all sockets (not just connected)
    -l  lay listening server sockets
    -t  TCP sockets
    -u  UDP sockets
    -w  raw sockets
    -x  unix sockets
    -e  extended info
    -n  don't resolve names
    -W  wide display
    -p  PID/Program name for sockets
*/

#define FOR_netstat
#include "toys.h"
#include <net/route.h>

GLOBALS(
  struct num_cache *inodes;
  int some_process_unidentified;
);

typedef union _iaddr {
  unsigned u;
  unsigned char b[4];
} iaddr;

typedef union _iaddr6 {
  struct {
    unsigned a;
    unsigned b;
    unsigned c;
    unsigned d;
  } u;
  unsigned char b[16];
} iaddr6;

#define ADDR_LEN (INET6_ADDRSTRLEN + 1 + 5 + 1)//IPv6 addr len + : + port + '\0'

/*
 * For TCP/UDP/RAW display data.
 */
static void display_data(unsigned rport, char *label,
                         unsigned rxq, unsigned txq, char *lip, char *rip,
                         unsigned state, unsigned uid, unsigned long inode)
{
  char *ss_state = "UNKNOWN", buf[12];
  char *state_label[] = {"", "ESTABLISHED", "SYN_SENT", "SYN_RECV", "FIN_WAIT1",
                         "FIN_WAIT2", "TIME_WAIT", "CLOSE", "CLOSE_WAIT",
                         "LAST_ACK", "LISTEN", "CLOSING", "UNKNOWN"};
  char user[11];
  struct passwd *pw;

  if (!strcmp(label, "tcp")) {
    int sz = ARRAY_LEN(state_label);
    if (!state || state >= sz) state = sz-1;
    ss_state = state_label[state];
  } else if (!strcmp(label, "udp")) {
    if (state == 1) ss_state = state_label[state];
    else if (state == 7) ss_state = "";
  } else if (!strcmp(label, "raw")) sprintf(ss_state = buf, "%u", state);

  if (!(toys.optflags & FLAG_n) && (pw = getpwuid(uid)))
    snprintf(user, sizeof(user), "%s", pw->pw_name);
  else snprintf(user, sizeof(user), "%d", uid);

  xprintf("%3s   %6d %6d ", label, rxq, txq);
  xprintf((toys.optflags & FLAG_W) ? "%-51.51s %-51.51s " : "%-23.23s %-23.23s "
           , lip, rip);
  xprintf("%-11s", ss_state);
  if ((toys.optflags & FLAG_e)) xprintf(" %-10s %-11ld", user, inode);
  if ((toys.optflags & FLAG_p)) {
    struct num_cache *nc = get_num_cache(TT.inodes, inode);

    printf(" %s", nc ? nc->data : "-");
  }
  xputc('\n');
}

/*
 * For TCP/UDP/RAW show data.
 */
static void show_data(unsigned rport, char *label, unsigned rxq, unsigned txq,
                      char *lip, char *rip, unsigned state, unsigned uid,
                      unsigned long inode)
{
  if (toys.optflags & FLAG_l) {
    if (!rport && (state & 0xA))
      display_data(rport, label, rxq, txq, lip, rip, state, uid, inode);
  } else if (toys.optflags & FLAG_a)
    display_data(rport, label, rxq, txq, lip, rip, state, uid, inode);
  //rport && (TCP | UDP | RAW)
  else if (rport & (0x10 | 0x20 | 0x40))
    display_data(rport, label, rxq, txq, lip, rip, state, uid, inode);
}

/*
 * used to get service name.
 */
static char *get_servname(int port, char *label)
{
  int lport = htons(port);

  if (!lport) return xmprintf("%s", "*");
  struct servent *ser = getservbyport(lport, label);
  if (ser) return xmprintf("%s", ser->s_name);
  return xmprintf("%u", (unsigned)ntohs(lport));
}

/*
 * used to convert address into text format.
 */
static void addr2str(int af, void *addr, unsigned port, char *buf, char *label)
{
  char ip[ADDR_LEN] = {0,};
  if (!inet_ntop(af, addr, ip, ADDR_LEN)) {
    *buf = '\0';
    return;
  }
  size_t iplen = strlen(ip);
  if (!port) {
    strncat(ip+iplen, ":*", ADDR_LEN-iplen-1);
    memcpy(buf, ip, ADDR_LEN);
    return;
  }

  if (!(toys.optflags & FLAG_n)) {
    struct addrinfo hints, *result, *rp;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = af;

    if (!getaddrinfo(ip, NULL, &hints, &result)) {
      char hbuf[NI_MAXHOST] = {0,}, sbuf[NI_MAXSERV] = {0,};
      socklen_t sock_len;
      char *sname = NULL;
      int plen = 0;

      if (af == AF_INET) sock_len = sizeof(struct sockaddr_in);
      else sock_len = sizeof(struct sockaddr_in6);

      for (rp = result; rp; rp = rp->ai_next)
        if (!getnameinfo(rp->ai_addr, sock_len, hbuf, sizeof(hbuf), sbuf,
            sizeof(sbuf), NI_NUMERICSERV))
          break;

      freeaddrinfo(result);
      sname = get_servname(port, label);
      plen = strlen(sname);
      if (*hbuf) {
        memset(ip, 0, ADDR_LEN);
        memcpy(ip, hbuf, (ADDR_LEN - plen - 2));
        iplen = strlen(ip);
      }
      snprintf(ip + iplen, ADDR_LEN-iplen, ":%s", sname);
      free(sname);
    }
  }
  else snprintf(ip+iplen, ADDR_LEN-iplen, ":%d", port);
  memcpy(buf, ip, ADDR_LEN);
}

/*
 * display ipv4 info for TCP/UDP/RAW.
 */
static void show_ipv4(char *fname, char *label)
{
  FILE *fp = fopen(fname, "r");
  if (!fp) {
     perror_msg("'%s'", fname);
     return;
  }

  if(!fgets(toybuf, sizeof(toybuf), fp)) return; //skip header.

  while (fgets(toybuf, sizeof(toybuf), fp)) {
    char lip[ADDR_LEN] = {0,}, rip[ADDR_LEN] = {0,};
    iaddr laddr, raddr;
    unsigned lport, rport, state, txq, rxq, num, uid;
    unsigned long inode;

    int nitems = sscanf(toybuf, " %d: %x:%x %x:%x %x %x:%x %*X:%*X %*X %d %*d %ld",
                        &num, &laddr.u, &lport, &raddr.u, &rport, &state, &txq,
                        &rxq, &uid, &inode);
    if (nitems == 10) {
      addr2str(AF_INET, &laddr, lport, lip, label);
      addr2str(AF_INET, &raddr, rport, rip, label);
      show_data(rport, label, rxq, txq, lip, rip, state, uid, inode);
    }
  }//End of While
  fclose(fp);
}

/*
 * display ipv6 info for TCP/UDP/RAW.
 */
static void show_ipv6(char *fname, char *label)
{
  FILE *fp = fopen(fname, "r");
  if (!fp) {
     perror_msg("'%s'", fname);
     return;
  }

  if(!fgets(toybuf, sizeof(toybuf), fp)) return; //skip header.

  while (fgets(toybuf, sizeof(toybuf), fp)) {
    char lip[ADDR_LEN] = {0,}, rip[ADDR_LEN] = {0,};
    iaddr6 laddr6, raddr6;
    unsigned lport, rport, state, txq, rxq, num, uid;
    unsigned long inode;

    int nitems = sscanf(toybuf, " %d: %8x%8x%8x%8x:%x %8x%8x%8x%8x:%x %x %x:%x "
                                "%*X:%*X %*X %d %*d %ld",
                        &num, &laddr6.u.a, &laddr6.u.b, &laddr6.u.c,
                        &laddr6.u.d, &lport, &raddr6.u.a, &raddr6.u.b,
                        &raddr6.u.c, &raddr6.u.d, &rport, &state, &txq, &rxq,
                        &uid, &inode);
    if (nitems == 16) {
      addr2str(AF_INET6, &laddr6, lport, lip, label);
      addr2str(AF_INET6, &raddr6, rport, rip, label);
      show_data(rport, label, rxq, txq, lip, rip, state, uid, inode);
    }
  }//End of While
  fclose(fp);
}

static void show_unix_sockets(void)
{
  char *types[] = {"","STREAM","DGRAM","RAW","RDM","SEQPACKET","DCCP","PACKET"},
       *states[] = {"","LISTENING","CONNECTING","CONNECTED","DISCONNECTING"},
       *s, *ss;
  unsigned long refcount, flags, type, state, inode;
  FILE *fp = xfopen("/proc/net/unix", "r");

  if(!fgets(toybuf, sizeof(toybuf), fp)) return; //skip header.

  while (fgets(toybuf, sizeof(toybuf), fp)) {
    unsigned offset = 0;

    // count = 6 or 7 (first field ignored, sockets don't always have filenames)
    if (6<sscanf(toybuf, "%*p: %lX %*X %lX %lX %lX %lu %n",
      &refcount, &flags, &type, &state, &inode, &offset))
        continue;

    // Linux exports only SO_ACCEPTCON since 2.3.15pre3 in 1999, but let's
    // filter in case they add more someday.
    flags &= 1<<16;

    // Only show unconnected listening sockets with -a
    if (state==1 && flags && !(toys.optflags&FLAG_a)) continue;

    if (type==10) type = 7; // move SOCK_PACKET into line
    if (type>ARRAY_LEN(types)) type = 0;
    if (state>ARRAY_LEN(states) || (state==1 && !flags)) state = 0;
    sprintf(toybuf, "[ %s]", flags ? "ACC " : "");

    printf("unix  %-6ld %-11s %-10s %-13s %8lu ",
      refcount, toybuf, types[type], states[state], inode);
    if (toys.optflags & FLAG_p) {
      struct num_cache *nc = get_num_cache(TT.inodes, inode);

      printf("%-19.19s", nc ? nc->data : "-");
    }

    if (offset) {
      if ((ss = strrchr(s = toybuf+offset, '\n'))) *ss = 0;
      xprintf("%s", s);
    }
    xputc('\n');
  }

  fclose(fp);
}

static int scan_pids(struct dirtree *node)
{
  char *s = toybuf+256;
  struct dirent *entry;
  DIR *dp;
  int pid, dirfd;

  if (!node->parent) return DIRTREE_RECURSE;
  if (!(pid = atol(node->name))) return 0;

  sprintf(toybuf, "/proc/%d/cmdline", pid);
  if (!(readfile(toybuf, toybuf, 256))) return 0;

  sprintf(s, "%d/fd", pid);
  if (-1==(dirfd = openat(dirtree_parentfd(node), s, O_RDONLY))) return 0;
  if (!(dp = fdopendir(dirfd))) {
    close(dirfd);

    return 0;
  }

  while ((entry = readdir(dp))) {
    s = toybuf+256;
    if (!readlinkat0(dirfd, entry->d_name, s, sizeof(toybuf)-256)) continue;
    // Can the "[0000]:" happen in a modern kernel?
    if (strstart(&s, "socket:[") || strstart(&s, "[0000]:")) {
      long long ll = atoll(s);

      sprintf(s, "%d/%s", pid, getbasename(toybuf));
      add_num_cache(&TT.inodes, ll, s, strlen(s)+1);
    }
  }
  closedir(dp);

  return 0;
}

/*
 * extract inet4 route info from /proc/net/route file and display it.
 */
static void display_routes(void)
{
  static const char flagchars[] = "GHRDMDAC";
  static const unsigned flagarray[] = {
    RTF_GATEWAY, RTF_HOST, RTF_REINSTATE, RTF_DYNAMIC, RTF_MODIFIED
  };
  unsigned long dest, gate, mask;
  int flags, ref, use, metric, mss, win, irtt;
  char *out = toybuf, *flag_val;
  char iface[64]={0};
  FILE *fp = xfopen("/proc/net/route", "r");

  if(!fgets(toybuf, sizeof(toybuf), fp)) return; //skip header.

  xprintf("Kernel IP routing table\n"
          "Destination     Gateway         Genmask         Flags %s Iface\n",
          !(toys.optflags&FLAG_e) ? "  MSS Window  irtt" : "Metric Ref    Use");

  while (fgets(toybuf, sizeof(toybuf), fp)) {
     char *destip = 0, *gateip = 0, *maskip = 0;

     if (11 != sscanf(toybuf, "%63s%lx%lx%X%d%d%d%lx%d%d%d", iface, &dest,
       &gate, &flags, &ref, &use, &metric, &mask, &mss, &win, &irtt))
         break;

    // skip down interfaces.
    if (!(flags & RTF_UP)) continue;

// TODO /proc/net/ipv6_route

    if (dest) {
      if (inet_ntop(AF_INET, &dest, out, 16)) destip = out;
    } else destip = (toys.optflags&FLAG_n) ? "0.0.0.0" : "default";
    out += 16;

    if (gate) {
      if (inet_ntop(AF_INET, &gate, out, 16)) gateip = out;
    } else gateip = (toys.optflags&FLAG_n) ? "0.0.0.0" : "*";
    out += 16;

// TODO /24
    //For Mask
    if (inet_ntop(AF_INET, &mask, out, 16)) maskip = out;
    else maskip = "?";
    out += 16;

    //Get flag Values
    flag_val = out;
    *out++ = 'U';
    for (dest = 0; dest < ARRAY_LEN(flagarray); dest++)
      if (flags&flagarray[dest]) *out++ = flagchars[dest];
    *out = 0;
    if (flags & RTF_REJECT) *flag_val = '!';

    xprintf("%-15.15s %-15.15s %-16s%-6s", destip, gateip, maskip, flag_val);
    if (!(toys.optflags & FLAG_e))
      xprintf("%5d %-5d %6d %s\n", mss, win, irtt, iface);
    else xprintf("%-6d %-2d %7d %s\n", metric, ref, use, iface);
  }

  fclose(fp);
}

void netstat_main(void)
{
  int tuwx = FLAG_t|FLAG_u|FLAG_w|FLAG_x;
  char *type = "w/o";

  if (!(toys.optflags&(FLAG_r|tuwx))) toys.optflags |= tuwx;
  if (toys.optflags & FLAG_r) display_routes();
  if (!(toys.optflags&tuwx)) return;

  if (toys.optflags & FLAG_a) type = "established and";
  else if (toys.optflags & FLAG_l) type = "only";

  if (toys.optflags & FLAG_p) {
    dirtree_read("/proc", scan_pids);
    // TODO: we probably shouldn't warn if all the processes we're going to
    // list were identified.
    if (TT.some_process_unidentified)
      fprintf(stderr,
        "(Not all processes could be identified, non-owned process info\n"
        " will not be shown, you would have to be root to see it all.)\n");
  }

  if (toys.optflags&(FLAG_t|FLAG_u|FLAG_w)) {
    int pad = (toys.optflags&FLAG_W) ? -51 : -23;

    printf("Active %s (%s servers)\n", "Internet connections", type);

    printf("Proto Recv-Q Send-Q %*s %*s State      ", pad, "Local Addres",
      pad, "Foreign Address");
    if (toys.optflags & FLAG_e) printf(" User       Inode      ");
    if (toys.optflags & FLAG_p) printf(" PID/Program Name");
    xputc('\n');

    if (toys.optflags & FLAG_t) {//For TCP
      show_ipv4("/proc/net/tcp",  "tcp");
      show_ipv6("/proc/net/tcp6", "tcp");
    }
    if (toys.optflags & FLAG_u) {//For UDP
      show_ipv4("/proc/net/udp",  "udp");
      show_ipv6("/proc/net/udp6", "udp");
    }
    if (toys.optflags & FLAG_w) {//For raw
      show_ipv4("/proc/net/raw",  "raw");
      show_ipv6("/proc/net/raw6", "raw");
    }
  }

  if (toys.optflags & FLAG_x) {
    xprintf("Active %s (%s servers)\n", "UNIX domain sockets", type);

    xprintf("Proto RefCnt Flags       Type       State           I-Node %s Path\n",
      (toys.optflags&FLAG_p) ? "PID/Program Name" : "I-Node");
    show_unix_sockets();
  }

  if ((toys.optflags & FLAG_p) && CFG_TOYBOX_FREE)
    llist_traverse(TT.inodes, free);
  toys.exitval = 0;
}
