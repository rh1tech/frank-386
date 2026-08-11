#include <i86.h>
#include "doomdef.h"

typedef struct
{
	unsigned        edi, esi, ebp, reserved, ebx, edx, ecx, eax;
	unsigned short  flags, es, ds, fs, gs, ip, cs, sp, ss;
} dpmiregs_t;

extern  dpmiregs_t      dpmiregs;

extern union REGS regs;

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long LONG;

typedef struct IPXPacketStructure
{
     WORD    PacketCheckSum;         /* high-low */
     WORD    PacketLength;           /* high-low */
     BYTE    PacketTransportControl;
     BYTE    PacketType;

     BYTE    dNetwork[4];            /* high-low */
     BYTE    dNode[6];               /* high-low */
     BYTE    dSocket[2];             /* high-low */

     BYTE    sNetwork[4];            /* high-low */
     BYTE    sNode[6];               /* high-low */
     BYTE    sSocket[2];             /* high-low */
} IPXPacket;


typedef struct
{
     BYTE    network[4];             /* high-low */
     BYTE    node[6];                /* high-low */
} localadr_t;

typedef struct
{
     BYTE    node[6];                /* high-low */
} nodeadr_t;

typedef struct ECBStructure
{
     WORD    Link[2];                /* offset-segment */
     WORD    ESRAddress[2];          /* offset-segment */
     BYTE    InUseFlag;
     BYTE    CompletionCode;
     WORD    ECBSocket;              /* high-low */
     BYTE    IPXWorkspace[4];        /* N/A */
     BYTE    DriverWorkspace[12];    /* N/A */
     BYTE    ImmediateAddress[6];    /* high-low */
     WORD    FragmentCount;          /* low-high */

     WORD    fAddress[2];            /* offset-segment */
     WORD    fSize;                  /* low-high */

     //WORD    f2Address[2];            /* offset-segment */
     //WORD    f2Size;                  /* low-high */
} ECB;

typedef struct
{
    ECB             ecb;
    IPXPacket       ipx;

    doomdata_t          data;
    int checksum;
} packet_t;

#define NUMPACKETS      10              // max outstanding packets before loss

unsigned int ipx_ptr[2];

localadr_t *localadr;

packet_t *packets;

int socketid = 0x869c;

boolean gotpacket;

int localnetid;
int remotenetid;

int OpenSocket(short a1)
{
    regs.w.bx = 0;
    regs.h.al = 0;
    regs.w.dx = a1;
    int386(0x7a, &regs, &regs);
    if (regs.h.al)
        I_Error("OpenSocket: 0x%x", regs.h.al);

    return regs.w.dx;
}

void ListenForPacket(ECB *ecb)
{
    memset(&dpmiregs, 0, sizeof(dpmiregs));
    dpmiregs.esi = (unsigned)(ecb) & 15;
    dpmiregs.es = (unsigned)(ecb) >> 4;
    dpmiregs.ebx = 4;
    dpmiregs.ip = ipx_ptr[0];
    dpmiregs.cs = ipx_ptr[1];
    DPMIInt(0x7a);
    if (dpmiregs.eax & 0xff)
        I_Error("ListenForPacket: 0x%x", dpmiregs.eax & 0xff);
}

void GetLocalAddress(void)
{
    localadr = I_AllocLow(sizeof(*localadr));
    memset(&dpmiregs, 0, sizeof(dpmiregs));
    dpmiregs.esi = (unsigned)(localadr) & 15;
    dpmiregs.es = (unsigned)(localadr) >> 4;
    dpmiregs.ebx = 9;
    dpmiregs.ip = ipx_ptr[0];
    dpmiregs.cs = ipx_ptr[1];
    DPMIInt(0x7a);
    if (dpmiregs.eax & 0xff)
        I_Error("Get inet addr: 0x%x", dpmiregs.eax & 0xff);

    localnetid = *(int*)&localadr->node[0];
    localnetid ^= *(int*)&localadr->node[2];
    printf("localnetid: 0x%x\n", localnetid);
}

void InitNetwork(void)
{
    int i;
    int j;
    int id;
    //packet_t *packet;
//
// get IPX function address
//
    memset(&dpmiregs, 0, sizeof(dpmiregs));
    dpmiregs.eax = 0x7a00;
    DPMIInt(0x2f);
    if ((dpmiregs.eax & 0xff) != 0xff)
        I_Error("IPX not detected\n");

    ipx_ptr[0] = dpmiregs.edi & 0xffff;
    ipx_ptr[1] = dpmiregs.es;

    packets = I_AllocLow(sizeof(packet_t) * NUMPACKETS);
    i = M_CheckParm("-port");
    if (i > 0 && i < myargc-1)
    {
        socketid = atoi(myargv[i + 1]);
        printf("using alternate port %i for network\n", socketid);
    }

//
// allocate a socket for sending and receiving
//
    id = OpenSocket ( (socketid>>8) + ((socketid&255)<<8) );

    GetLocalAddress();

//
// set up several receiving ECBs
//
    memset (packets,0,NUMPACKETS*sizeof(packet_t));

    for (i=1 ; i<NUMPACKETS ; i++)
    {
        packets[i].ecb.ECBSocket = id;
        packets[i].ecb.InUseFlag = 29;
        packets[i].ecb.FragmentCount = 1;
        packets[i].ecb.fAddress[0] = (unsigned)(&packets[i].ipx) & 15;
        packets[i].ecb.fAddress[1] = (unsigned)(&packets[i].ipx) >> 4;
        packets[i].ecb.fSize = sizeof(packet_t)-sizeof(ECB);

        ListenForPacket (&packets[i].ecb);
    }

//
// set up a sending ECB
//
    memset (&packets[0],0,sizeof(packets[0]));

    packets[0].ecb.ECBSocket = id;
    packets[0].ecb.FragmentCount = 1;
    packets[0].ecb.fAddress[0] = (unsigned)(&packets[0].ipx) & 15;
    packets[0].ecb.fAddress[1] = (unsigned)(&packets[0].ipx) >> 4;
    packets[0].ecb.fSize = sizeof(packet_t)-sizeof(ECB);
    for (j=0 ; j<6 ; j++)
    {
        packets[0].ipx.dNode[j] = 255;
        packets[0].ecb.ImmediateAddress[j] = 255;
    }
    packets[0].ipx.dSocket[0] = id&255;
    packets[0].ipx.dSocket[1] = id>>8;
}

/*
==============
=
= SendPacket
=
==============
*/

void SendPacket (void)
{
    int sum;
    int i;
    if (!netgame)
    {
        gotpacket = true;
        return;
    }

    do
    {
    } while (packets[0].ecb.InUseFlag);

    memcpy(&packets[0].data, &netbuffer, sizeof(netbuffer));
    sum = 0;

    for (i = 0; i < sizeof(doomdata_t); i++)
    {
        sum += ((unsigned char*)&packets[0].data)[i];
    }

    packets[0].checksum = sum;

    memset(&dpmiregs, 0, sizeof(dpmiregs));

    dpmiregs.esi = (unsigned)(&packets[0]) & 15;
    dpmiregs.es = (unsigned)(&packets[0]) >> 4;
    dpmiregs.ebx = 3;
    dpmiregs.ip = ipx_ptr[0];
    dpmiregs.cs = ipx_ptr[1];
    DPMIInt(0x7a);
    if (dpmiregs.eax & 0xff)
        I_Error("SendPacket: 0x%x", dpmiregs.eax & 0xff);
}

/*
==============
=
= GetPacket
=
= Returns false if no packet is waiting
=
==============
*/

int GetPacket (void)
{
    int             packetnum;
    int             i, j;
    int sum;
    long           besttic;
    packet_t* packet;

    if (!netgame)
    {
        if (gotpacket)
        {
            gotpacket = false;
            return 1;
        }
        return 0;
    }
// if multiple packets are waiting, return them in order by time

    besttic = MAXLONG;
    packetnum = -1;

    for ( i = 1 ; i < NUMPACKETS ; i++)
    {
        if (packets[i].ecb.InUseFlag)
        {
            continue;
        }

        if (packets[i].data.tic < besttic)
        {
            besttic = packets[i].data.tic;
            packetnum = i;
        }
    }

    if (besttic == MAXLONG)
        return 0;                           // no packets

    if (packetnum == -1)
        I_Error("GetPacket: packetnum == -1");
//
// got a good packet
//
    if (packets[packetnum].ecb.CompletionCode)
        I_Error ("GetPacket: ecb.ComletionCode = 0x%x",packets[packetnum].ecb.CompletionCode);

    remotenetid = *(int*)&packets[packetnum].ipx.sNode[0];
    remotenetid ^= *(int*)&packets[packetnum].ipx.sNode[2];

    sum = 0;
    for (j = 0; j < sizeof(doomdata_t); j++)
    {
        sum += ((unsigned char*)&packets[packetnum].data)[j];
    }

    if (packets[packetnum].checksum != sum)
        I_Error("GetPacket: bad checksum");

    memcpy(&netbuffer, &packets[packetnum].data, sizeof(netbuffer));
    ListenForPacket(&packets[packetnum].ecb);
    return 1;
}
