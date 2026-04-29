#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static struct spinlock netlock;

#define UDP_PORT_BUF_MAX 16
struct udp_port_info{
    struct proc *p;
    char *buf[UDP_PORT_BUF_MAX];
    uint8 buf_used;
    uint8 recv_index;
    uint8 send_index;
};

struct udp_port_info_run {
    struct udp_port_info_run *next;
};
static struct udp_port_info_run *freelist;

static struct udp_port_info **udp_port_info_list[128];

void
netinit(void)
{
  initlock(&netlock, "netlock");
  freelist = (struct udp_port_info_run *)0;
}

static void free_udp_port_info(struct udp_port_info *info){
    struct udp_port_info_run *r;

    r = (struct udp_port_info_run*)info;

    r->next = freelist;
    freelist = r;
}

static struct udp_port_info *alloc_udp_port_info(void){
    struct udp_port_info_run *r;

    if(!freelist){
        void *mem = kalloc();
        if(!mem)
            return 0;
        memset(mem, 0, PGSIZE);
        for(struct udp_port_info *temp = (struct udp_port_info *)mem;((uint64)temp + sizeof(struct udp_port_info)) < (uint64)mem + PGSIZE; temp++){
            free_udp_port_info(temp);
        }
    }

    r = freelist;
    if(r)
        freelist = r->next;

    if(r){
        ((struct udp_port_info *)r)->p = myproc();
        ((struct udp_port_info *)r)->buf_used = 0;
        ((struct udp_port_info *)r)->recv_index = 0;
        ((struct udp_port_info *)r)->send_index = 0;
    }
    return (struct udp_port_info *)r;
}

static struct udp_port_info **find_udp_port_info(uint16 port){
    struct udp_port_info **pp_udp_port_info = udp_port_info_list[port / 512];

    if(!pp_udp_port_info){
        return 0;
    }
    return &pp_udp_port_info[port % 512];
}

//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64
sys_bind(void)
{
  //
  // Your code here.
  //

    int port;

    argint(0, &port);

    acquire(&netlock);
    struct udp_port_info **pp_udp_port_info = udp_port_info_list[port / 512];

    if(!pp_udp_port_info){
        udp_port_info_list[port / 512] = (struct udp_port_info **)kalloc();
        if(!udp_port_info_list[port / 512])
            panic("sys_bind udp_port_info_list don't have page");
        pp_udp_port_info = udp_port_info_list[port / 512];
        memset(pp_udp_port_info, 0, PGSIZE);
    }
    struct udp_port_info *p_udp_port_info = pp_udp_port_info[port % 512];
    if(p_udp_port_info){
        release(&netlock);
        return -1;
    }else{
        pp_udp_port_info[port % 512] = alloc_udp_port_info();
        if(!pp_udp_port_info[port % 512])
            panic("sys_bind udp_port_info don't have cache");
    }
    release(&netlock);
    return 0;
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  //
  // Optional: Your code here.
  //

    int port;

    argint(0, &port);

    acquire(&netlock);
    struct udp_port_info **pp_udp_port_info = find_udp_port_info(port);
    if((!pp_udp_port_info) || (!*pp_udp_port_info)){
        release(&netlock);
        return -1;
    }
    free_udp_port_info(*pp_udp_port_info);
    *pp_udp_port_info = 0;
    release(&netlock);
    return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//
uint64
sys_recv(void)
{
  //
  // Your code here.
  //

    struct proc *p = myproc();
    int dport;
    uint64 src;
    uint64 sport;
    uint64 bufaddr;
    int maxlen;

    argint(0, &dport);
    argaddr(1, &src);
    argaddr(2, &sport);
    argaddr(3, &bufaddr);
    argint(4, &maxlen);

    acquire(&netlock);
    struct udp_port_info **pp_udp_port_info = find_udp_port_info(dport);
    if(!pp_udp_port_info){
        release(&netlock);
        return -1;
    }
    struct udp_port_info *p_udp_port_info = *pp_udp_port_info;
    if(!p_udp_port_info){
        release(&netlock);
        return -1;
    }
    if(p_udp_port_info->p != p){
        release(&netlock);
        return -1;
    }
    if(p_udp_port_info->buf_used == 0){
        sleep(&p_udp_port_info->buf_used, &netlock);
    }
    if(p_udp_port_info->buf_used == 0)
        panic("sys_recv error wakeup");
    uint32 recv_dst;
    uint16 recv_dport;
    struct eth *eth = (struct eth *) p_udp_port_info->buf[p_udp_port_info->send_index];
    struct ip *ip = (struct ip *)(eth + 1);
    struct udp *udp = (struct udp *)(ip + 1);
    recv_dst   = ntohl(ip->ip_src);
    recv_dport = ntohs(udp->sport);
    int ret = copyout(p->pagetable, src, (char *)(&recv_dst), 4);
    if(ret){
        goto free_info;
    }
    ret = copyout(p->pagetable, sport, (char *)(&recv_dport), 2);
    if(ret){
        goto free_info;
    }
    uint64 len = (ntohs(udp->ulen) - sizeof(struct udp));
    ret = copyout(p->pagetable, bufaddr, (char *)(udp + 1), (len > maxlen) ? maxlen : len);
    if(!ret){
        ret = (len > maxlen) ? maxlen : len;
    }
free_info:
    p_udp_port_info->buf_used--;
    kfree(p_udp_port_info->buf[p_udp_port_info->send_index]);
    p_udp_port_info->buf[p_udp_port_info->send_index] = 0;
    p_udp_port_info->send_index = ((p_udp_port_info->send_index + 1) % UDP_PORT_BUF_MAX);
    release(&netlock);

    return ret;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45; // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }
    uint16 ttl = ip->ip_ttl;
    uint16 ipsum = ip->ip_sum;
    ip->ip_ttl = 0;
    ip->ip_sum = udp->ulen;
    udp->sum = in_cksum((unsigned char *)&(ip->ip_ttl), 20 + len);
    ip->ip_ttl = ttl;
    ip->ip_sum = ipsum;

    int ret = e1000_transmit(buf, total);
    if(ret){
        kfree(buf);
    }

  return 0;
}

static void udp_rx(char *buf, int len){
    struct eth *eth = (struct eth *) buf;
    struct ip *ip = (struct ip *)(eth + 1);
    struct udp *udp= (struct udp *)(ip + 1);
    acquire(&netlock);
    struct udp_port_info **pp_udp_port_info = find_udp_port_info(ntohs(udp->dport));
    if(!pp_udp_port_info){
        printf("udp_rx: drop because !pp_udp_port_info, port : %d\n", ntohs(udp->dport));
        goto error_out;
    }
    struct udp_port_info *p_udp_port_info = *pp_udp_port_info;
    if(!p_udp_port_info){
        printf("udp_rx: drop because !p_udp_port_info, port : %d\n", ntohs(udp->dport));
        goto error_out;
    }
    uint16 ulen = ntohs(udp->ulen) - sizeof(struct udp);
    if(ulen + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp) > len){
        printf("udp_rx: drop because ulen + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp) > len\n");
        goto error_out;
    }
    ip->ip_ttl = 0;
    ip->ip_sum = udp->ulen;
    unsigned short udp_csum = in_cksum((unsigned char *)&(ip->ip_ttl), 20 + ulen);
    if(udp_csum != 0){
        printf("udp_rx: drop because udp_csum != 0\n");
        goto error_out;
    }
    if(p_udp_port_info->buf_used == UDP_PORT_BUF_MAX){
        printf("udp_rx: drop because p_udp_port_info->buf_used == UDP_PORT_BUF_MAX\n");
        goto error_out;
    }
    p_udp_port_info->buf_used++;
    p_udp_port_info->buf[p_udp_port_info->recv_index] = buf;
    p_udp_port_info->recv_index = ((p_udp_port_info->recv_index + 1) % UDP_PORT_BUF_MAX);
    wakeup(&p_udp_port_info->buf_used);
    release(&netlock);
    return;
error_out:
    kfree(buf);
    release(&netlock);
}

void
ip_rx(char *buf, int len)
{
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  //
  // Your code here.
  //

    struct eth *eth = (struct eth *) buf;
    struct ip *ip = (struct ip *)(eth + 1);
    unsigned short ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));
    if(ip_sum != 0){
        kfree(buf);
        return;
    }
    if(ip->ip_p == IPPROTO_ICMP){}
    else if(ip->ip_p == IPPROTO_TCP){}
    else if(ip->ip_p == IPPROTO_UDP){
        if(len >= sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp))
            udp_rx(buf, len);
        else
            kfree(buf);
    }else{
        printf("now recv a %d\n", ip->ip_p);
        panic("ip_rx unknow IPPROTO");
    }
}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN); // ethernet destination = query source
  memmove(eth->shost, local_mac, ETHADDR_LEN); // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

    int ret = e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));
    if(ret){
        kfree(buf);
    }

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if(len >= sizeof(struct eth) + sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= sizeof(struct eth) + sizeof(struct ip) &&
     ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
