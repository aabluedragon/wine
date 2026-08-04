/*
 * ping program
 *
 * Copyright (C) 2010 Trey Hunner
 * Copyright (C) 2018 Isira Seneviratne
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "winsock2.h"
#include "ws2tcpip.h"
#include "iphlpapi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <icmpapi.h>
#include <limits.h>

#include <windows.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ping);

static void usage(void)
{
    printf("Usage: ping [-t] [-a] [-n count] [-l buffer_length] [-f] [-i TTL]\n"
           "            [-w timeout] [-S srcaddr] [-4] [-6] target_name\n\n"
           "Options:\n"
           "    -t  Ping the host until stopped.\n"
           "    -a  Resolve the address to a host name.\n"
           "    -n  Number of echo requests to send.\n"
           "    -l  Length of send buffer.\n"
           "    -f  Set the Don't Fragment flag.\n"
           "    -i  Time To Live.\n"
           "    -w  Timeout in milliseconds to wait for each reply.\n"
           "    -S  Source address to use.\n"
           "    -4  Use IPv4.\n"
           "    -6  Use IPv6.\n");
}

/* Every option that takes a value, so that a missing one is reported the same
 * way for all of them. */
static const char *option_value( int argc, char **argv, int *i )
{
    if (*i == argc - 1)
    {
        printf( "Missing value for option %s\n", argv[*i] );
        exit(1);
    }
    return argv[++(*i)];
}

int __cdecl main(int argc, char** argv)
{
    unsigned int n = 4, i, w = 4000, l = 32;
    int res;
    int rec = 0, lost = 0, min = INT_MAX, max = 0;
    IP_OPTION_INFORMATION options = { 128, 0, 0, 0, NULL }, *send_options = NULL;
    BOOL forever = FALSE, resolve = FALSE;
    unsigned long srcaddr = INADDR_ANY;
    WSADATA wsa;
    HANDLE icmp_file;
    unsigned long ipaddr;
    DWORD retval, reply_size;
    char *send_data, ip[100], *hostname = NULL, rtt[16], name[NI_MAXHOST];
    void *reply_buffer;
    struct in_addr addr;
    ICMP_ECHO_REPLY *reply;
    float avg = 0;
    struct hostent *remote_host;

    if (argc == 1)
    {
        usage();
        exit(1);
    }

    for (i = 1; i < argc; i++)
    {
        if (argv[i][0] == '-' || argv[i][0] == '/')
        {
            switch (argv[i][1])
            {
            case 'n':
                n = atoi(option_value( argc, argv, &i ));
                if (n == 0)
                {
                  printf("Bad value for option -n, valid range is from 1 to 4294967295.\n");
                  exit(1);
                }
                break;
            case 'w':
                w = atoi(option_value( argc, argv, &i ));
                if (w == 0)
                {
                    printf("Bad value for option -w.\n");
                    exit(1);
                }
                break;
            case 'l':
                l = atoi(option_value( argc, argv, &i ));
                if (l == 0)
                {
                    printf("Bad value for option -l.\n");
                    exit(1);
                }
                break;
            case 't':
                forever = TRUE;
                break;
            case 'a':
                resolve = TRUE;
                break;
            case 'f':
                options.Flags |= IP_FLAG_DF;
                send_options = &options;
                break;
            case 'i':
                res = atoi(option_value( argc, argv, &i ));
                if (res < 1 || res > 255)
                {
                    printf("Bad value for option -i, valid range is from 1 to 255.\n");
                    exit(1);
                }
                options.Ttl = res;
                send_options = &options;
                break;
            case 'S':
                srcaddr = inet_addr(option_value( argc, argv, &i ));
                if (srcaddr == INADDR_NONE)
                {
                    printf("Bad value for option -S.\n");
                    exit(1);
                }
                break;
            case '4':
                /* The only family this sends on. */
                break;
            case '6':
                printf("Only IPv4 echo requests are supported.\n");
                exit(1);
            case '?':
                usage();
                exit(1);
            default:
                printf( "Bad option %s\n", argv[i] );
                usage();
                exit(1);
            }
        }
        else
        {
            if (hostname)
            {
                printf( "Bad argument %s\n", argv[i] );
                exit(1);
            }
            hostname = argv[i];
        }
    }

    if (!hostname)
    {
        printf("Pass a host name.\n");
        return 1;
    }

    res = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (res != 0)
    {
        printf("WSAStartup failed: %d\n", res);
        return 1;
    }

    remote_host = gethostbyname(hostname);
    if (remote_host == NULL)
    {
        printf("Ping request could not find host %s. Please check the name and try again.\n",
               hostname);
        return 1;
    }

    addr.s_addr = *(u_long *) remote_host->h_addr_list[0];
    strcpy(ip, inet_ntoa(addr));
    ipaddr = inet_addr(ip);
    if (ipaddr == INADDR_NONE)
    {
        printf("Could not get IP address of host %s.", hostname);
        return 1;
    }

    icmp_file = IcmpCreateFile();

    send_data = calloc(1, l);
    reply_size = sizeof(ICMP_ECHO_REPLY) + l + 8;
    /* The buffer has to hold 8 more bytes of data (the size of an ICMP error message). */
    reply_buffer = malloc(reply_size);
    if (reply_buffer == NULL)
    {
        printf("Unable to allocate memory to reply buffer.\n");
        return 1;
    }

    if (resolve)
    {
        SOCKADDR_IN sa = { 0 };

        sa.sin_family = AF_INET;
        sa.sin_addr = addr;
        if (!getnameinfo( (SOCKADDR *)&sa, sizeof(sa), name, sizeof(name), NULL, 0, 0 ))
            hostname = name;
    }

    printf("Pinging %s [%s] with %d bytes of data:\n", hostname, ip, l);
    for (i = 0; forever || i < n; i++)
    {
        SetLastError(0);
        retval = IcmpSendEcho2Ex(icmp_file, NULL, NULL, NULL, srcaddr, ipaddr, send_data, l,
            send_options, reply_buffer, reply_size, w);
        if (retval != 0)
        {
            reply = (ICMP_ECHO_REPLY *) reply_buffer;
            if (reply->RoundTripTime >= 1)
                sprintf(rtt, "=%ld", reply->RoundTripTime);
            else
                strcpy(rtt, "<1");
            printf("Reply from %s: bytes=%d time%sms TTL=%d\n", ip, l,
                rtt, reply->Options.Ttl);
            if (reply->RoundTripTime > max)
                max = reply->RoundTripTime;
            if (reply->RoundTripTime < min)
                min = reply->RoundTripTime;
            avg += reply->RoundTripTime;
            rec++;
        }
        else
        {
            if (GetLastError() == IP_REQ_TIMED_OUT)
                puts("Request timed out.");
            else
                puts("PING: transmit failed. General failure.");
            lost++;
        }
        if (forever || i < n - 1) Sleep(1000);
    }

    printf("\nPing statistics for %s\n", ip);
    printf("\tPackets: Sent = %d, Received = %d, Lost = %d (%.0f%% loss)\n",
        n, rec, lost, (float) lost / n * 100);
    if (rec != 0)
    {
        avg /= rec;
        printf("Approximate round trip times in milli-seconds:\n");
        printf("\tMinimum = %dms, Maximum = %dms, Average = %.0fms\n",
               min, max, avg);
    }

    free(reply_buffer);
    return 0;
}
