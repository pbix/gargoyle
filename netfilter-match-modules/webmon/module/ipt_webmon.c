/*  webmon --	A netfilter module to match URLs in HTTP requests 
 *  		This module can match using string match or regular expressions
 *  		Originally designed for use with Gargoyle router firmware (gargoyle-router.com)
 *
 *
 *  Copyright © 2008-2010 by Eric Bishop <eric@gargoyle-router.com>
 * 
 *  This file is free software: you may copy, redistribute and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation, either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  This file is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/if_ether.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <net/sock.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <linux/time.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>

#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netfilter_ipv4/ipt_webmon.h>

#include "webmon_deps/tree_map.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,21)
	#define ipt_register_match      xt_register_match
	#define ipt_unregister_match    xt_unregister_match
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	#include <linux/ktime.h>
#endif


#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22)
	#include <linux/ip.h>
#else
	#define skb_network_header(skb) (skb)->nh.raw 
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
	#include <linux/netfilter/x_tables.h>
#endif

#define STRIP "%d.%d.%d.%d"
#define IP2STR(x)   (x)&0xff,(x)>>8&0xff,(x)>>16&0xff,(x)>>24&0xff /*assumes network byte order*/

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Eric Bishop");
MODULE_DESCRIPTION("Monitor URL in HTTP Requests, designed for use with Gargoyle web interface (www.gargoyle-router.com)");



typedef struct qn
{
	uint32_t src_ip;
	char* value;
	struct timeval time;
	struct qn* next;
	struct qn* previous;	
} queue_node;

typedef struct
{
	queue_node* first;
	queue_node* last;
	int length;
} queue;

static string_map* domain_map = NULL;
static queue* recent_domains  = NULL;
static int max_domain_queue_length   = 5;
static int max_search_queue_length   = 5;

static spinlock_t webmon_lock = SPIN_LOCK_UNLOCKED;


static void update_queue_node_time(queue_node* update_node, queue* full_queue)
{
	struct timeval t;
	do_gettimeofday(&t);
	update_node->time = t;
	
	/* move to front of queue if not already at front of queue */
	if(update_node->previous != NULL)
	{
		queue_node* p = update_node->previous;
		queue_node* n = update_node->next;
		p->next = n;
		if(n != NULL)
		{
			n->previous = p;
		}
		else
		{
			full_queue->last = p;
		}
		update_node->previous = NULL;
		update_node->next = full_queue->first;
		full_queue->first->previous = update_node;
		full_queue->first = update_node;
	}
}

void add_queue_node(uint32_t src_ip, char* value, queue* full_queue, string_map* queue_index, char* queue_index_key, uint32_t max_queue_length )
{

	queue_node *new_node = (queue_node*)kmalloc(sizeof(queue_node), GFP_ATOMIC);
	char* dyn_value = kernel_strdup(value);
	struct timeval t;


	if(new_node == NULL || dyn_value == NULL)
	{
		if(dyn_value) { kfree(dyn_value); }
		if(new_node) { kfree(new_node); };

		return;
	}
	set_map_element(queue_index, queue_index_key, (void*)new_node);


	do_gettimeofday(&t);
	new_node->time = t;
	new_node->src_ip = src_ip;
	new_node->value = dyn_value;
	new_node->previous = NULL;
	
	new_node->next = full_queue->first;
	if(full_queue->first != NULL)
	{
		full_queue->first->previous = new_node;
	}
	full_queue->first = new_node;
	full_queue->last = (full_queue->last == NULL) ? new_node : full_queue->last ;
	full_queue->length = full_queue->length + 1;

	if( full_queue->length > max_queue_length )
	{
		queue_node *old_node = full_queue->last;
		full_queue->last = old_node->previous;
		full_queue->last->next = NULL;
		full_queue->first = old_node->previous == NULL ? NULL : full_queue->first; /*shouldn't be needed, but just in case...*/
		full_queue->length = full_queue->length - 1;
		
		sprintf(queue_index_key, STRIP"@%s", IP2STR(old_node->src_ip), old_node->value);
		remove_map_element(queue_index, queue_index_key);

		kfree(old_node->value);
		kfree(old_node);
	}

	/*
	queue_node* n = full_queue->first;
	while(n != NULL)
	{
		printf("%ld\t%s\t%s\t%s\n", (unsigned long)n->time, n->src_ip, n->dst_ip, n->domain);
		n = (queue_node*)n->next;
	}
	printf("\n\n");
	*/
}

void destroy_queue(queue* q)
{	
	queue_node *last_node = q->last;
	while(last_node != NULL)
	{
		queue_node *previous_node = last_node->previous;
		free(last_node->value);
		free(last_node);
		last_node = previous_node;
	}
	free(q);
}


int strnicmp(const char * cs,const char * ct,size_t count)
{
	register signed char __res = 0;

	while (count)
	{
		if ((__res = toupper( *cs ) - toupper( *ct++ ) ) != 0 || !*cs++)
		{
			break;
		}
		count--;
	}
	return __res;
}

char *strnistr(const char *s, const char *find, size_t slen)
{
	char c, sc;
	size_t len;


	if ((c = *find++) != '\0') 
	{
		len = strlen(find);
		do
		{
			do
			{
      				if (slen < 1 || (sc = *s) == '\0')
				{
      					return (NULL);
				}
      				--slen;
      				++s;
      			}
			while ( toupper(sc) != toupper(c));
      			
			if (len > slen)
			{
      				return (NULL);
			}
      		}
		while (strnicmp(s, find, len) != 0);
      		
		s--;
      	}
      	return ((char *)s);
}



/*
 * line is the line to be parsed -- it is not modified in any way
 * max_pieces indicates number of pieces to return, if negative this is determined dynamically
 * include_remainder_at_max indicates whether the last piece, when max pieces are reached, 
 * 	should be what it would normally be (0) or the entire remainder of the line (1)
 * 	if max_pieces < 0 this parameter is ignored
 *
 *
 * returns all non-separator pieces in a line
 * result is dynamically allocated, MUST be freed after call-- even if 
 * line is empty (you still get a valid char** pointer to to a NULL char*)
 */
char** split_on_separators(char* line, char* separators, int num_separators, int max_pieces, int include_remainder_at_max, unsigned long *num_pieces)
{
	char** split;
	
	*num_pieces = 0;
	if(line != NULL)
	{
		int split_index;
		int non_separator_found;
		char* dup_line;
		char* start;

		if(max_pieces < 0)
		{
			/* count number of separator characters in line -- this count + 1 is an upperbound on number of pieces */
			int separator_count = 0;
			int line_index;
			for(line_index = 0; line[line_index] != '\0'; line_index++)
			{
				int sep_index;
				int found = 0;
				for(sep_index =0; found == 0 && sep_index < num_separators; sep_index++)
				{
					found = separators[sep_index] == line[line_index] ? 1 : 0;
				}
				separator_count = separator_count+ found;
			}
			max_pieces = separator_count + 1;
		}
		split = (char**)malloc((1+max_pieces)*sizeof(char*));
		split_index = 0;
		split[split_index] = NULL;


		dup_line = strdup(line);
		start = dup_line;
		non_separator_found = 0;
		while(non_separator_found == 0)
		{
			int matches = 0;
			int sep_index;
			for(sep_index =0; sep_index < num_separators; sep_index++)
			{
				matches = matches == 1 || separators[sep_index] == start[0] ? 1 : 0;
			}
			non_separator_found = matches==0 || start[0] == '\0' ? 1 : 0;
			if(non_separator_found == 0)
			{
				start++;
			}
		}

		while(start[0] != '\0' && split_index < max_pieces)
		{
			/* find first separator index */
			int first_separator_index = 0;
			int separator_found = 0;
			while(	separator_found == 0 )
			{
				int sep_index;
				for(sep_index =0; separator_found == 0 && sep_index < num_separators; sep_index++)
				{
					separator_found = separators[sep_index] == start[first_separator_index] || start[first_separator_index] == '\0' ? 1 : 0;
				}
				if(separator_found == 0)
				{
					first_separator_index++;
				}
			}
			
			/* copy next piece to split array */
			if(first_separator_index > 0)
			{
				char* next_piece = NULL;
				if(split_index +1 < max_pieces || include_remainder_at_max <= 0)
				{
					next_piece = (char*)malloc((first_separator_index+1)*sizeof(char));
					memcpy(next_piece, start, first_separator_index);
					next_piece[first_separator_index] = '\0';
				}
				else
				{
					next_piece = strdup(start);
				}
				split[split_index] = next_piece;
				split[split_index+1] = NULL;
				split_index++;
			}


			/* find next non-separator index, indicating start of next piece */
			start = start+ first_separator_index;
			non_separator_found = 0;
			while(non_separator_found == 0)
			{
				int matches = 0;
				int sep_index;
				for(sep_index =0; sep_index < num_separators; sep_index++)
				{
					matches = matches == 1 || separators[sep_index] == start[0] ? 1 : 0;
				}
				non_separator_found = matches==0 || start[0] == '\0' ? 1 : 0;
				if(non_separator_found == 0)
				{
					start++;
				}
			}
		}
		free(dup_line);
		*num_pieces = split_index;
	}
	else
	{
		split = (char**)malloc((1)*sizeof(char*));
		split[0] = NULL;
	}
	return split;
}



static void extract_url(const unsigned char* packet_data, int packet_length, char* domain, char* path)
{

	int path_start_index;
	int path_end_index;
	int last_header_index;
	char last_two_buf[2];
	int end_found;
	char* domain_match;

	domain[0] = '\0';
	path[0] = '\0';


	/* get path portion of URL */
	path_start_index = (int)(strstr((char*)packet_data, " ") - (char*)packet_data);
	while( packet_data[path_start_index] == ' ')
	{
		path_start_index++;
	}
	path_end_index= (int)(strstr( (char*)(packet_data+path_start_index), " ") -  (char*)packet_data);
	if(path_end_index > 0) 
	{
		int path_length = path_end_index-path_start_index;
		path_length = path_length < 625 ? path_length : 624; /* prevent overflow */
		memcpy(path, packet_data+path_start_index, path_length);
		path[ path_length] = '\0';
	}
		
	/* get header length */
	last_header_index = 2;
	memcpy(last_two_buf,(char*)packet_data, 2);
	end_found = 0;
	while(end_found == 0 && last_header_index < packet_length)
	{
		char next = (char)packet_data[last_header_index];
		if(next == '\n')
		{
			end_found = last_two_buf[1] == '\n' || (last_two_buf[0] == '\n' && last_two_buf[1] == '\r') ? 1 : 0;
		}
		if(end_found == 0)
		{
			last_two_buf[0] = last_two_buf[1];
			last_two_buf[1] = next;
			last_header_index++;
		}
	}
		
	/* get domain portion of URL */
	domain_match = strnistr( (char*)packet_data, "Host:", last_header_index);
	if(domain_match != NULL)
	{
		int domain_end_index;
		domain_match = domain_match + 5; /* character after "Host:" */
		while(domain_match[0] == ' ')
		{
			domain_match = domain_match+1;
		}
		
		domain_end_index = 0;
		while(	domain_match[domain_end_index] != '\n' && 
			domain_match[domain_end_index] != '\r' && 
			domain_match[domain_end_index] != ' ' && 
			domain_match[domain_end_index] != ':' && 
			((char*)domain_match - (char*)packet_data)+domain_end_index < last_header_index 
			)
		{
			domain_end_index++;
		}
		memcpy(domain, domain_match, domain_end_index);
		domain_end_index = domain_end_index < 625 ? domain_end_index : 624; /* prevent overflow */
		domain[domain_end_index] = '\0';

		for(domain_end_index=0; domain[domain_end_index] != '\0'; domain_end_index++)
		{
			domain[domain_end_index] = (char)tolower(domain[domain_end_index]);
		}
	}
}

#ifdef CONFIG_PROC_FS

static void *webmon_proc_start(struct seq_file *seq, loff_t *loff_pos)
{
	static unsigned long counter = 0;

	/* beginning a new sequence ? */	
	if ( *loff_pos == 0 )
	{	
		/* yes => return a non null value to begin the sequence */
		return &counter;
	}
	else
	{
		/* no => it's the end of the sequence, return end to stop reading */
		*loff_pos = 0;
		return NULL;
	}
}

static void *webmon_proc_next(struct seq_file *seq, void *v, loff_t *pos)
{
	return NULL;
}


static void webmon_proc_stop(struct seq_file *seq, void *v)
{
	//don't need to do anything
}


static int webmon_proc_show(struct seq_file *s, void *v)
{
	spin_lock_bh(&webmon_lock);

	queue_node* next_node = recent_domains->last;
	while(next_node != NULL)
	{
		seq_printf(s, "%ld\t"STRIP"\t%s\n", (unsigned long)(next_node->time).tv_sec, IP2STR(next_node->src_ip), next_node->value);
		next_node = (queue_node*)next_node->previous;
	}
	spin_unlock_bh(&webmon_lock);

	return 0;
}


static struct seq_operations webmon_proc_sops = {
	.start = webmon_proc_start,
	.next  = webmon_proc_next,
	.stop  = webmon_proc_stop,
	.show  = webmon_proc_show
};

static int webmon_proc_open(struct inode *inode, struct file* file)
{
	return seq_open(file, &webmon_proc_sops);
}


static struct file_operations webmon_proc_fops = {
	.owner   = THIS_MODULE,
	.open    = webmon_proc_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release
};


#endif






static int ipt_webmon_set_ctl(struct sock *sk, int cmd, void *user, u_int32_t len)
{

	char* buffer = kmalloc(len, GFP_ATOMIC);
	if(buffer == NULL) /* check for malloc failure */
	{
		return 0;
	}
	copy_from_user(buffer, user, len);

	if(len > 1 + sizeof(uint32_t)) 
	{
		unsigned char type = buffer[0];
		uint32_t max_queue_length = *((uint32_t*)(buffer+1));
		char* data = buffer+1+sizeof(uint32_t);
		char newline_terminator[] = { '\n', '\r' };
		char whitespace_chars[] = { '\t', ' ' };

		spin_lock_bh(&webmon_lock);
		if(type == WEBMON_DOMAIN )
		{
			unsigned long num_lines;
			unsigned long line_index;
			unsigned long num_destroyed;
			char** lines = split_on_separators(data, newline_terminator, 2, -1, 0, &num_lines);
			max_domain_queue_length = max_queue_length;

			/* destroy and re-initialize queue and map */
			destroy_map(domain_map, DESTROY_MODE_IGNORE_VALUES, &num_destroyed);
			destroy_queue(recent_domains);
			recent_domains = (queue*)malloc(sizeof(queue));
			recent_domains->first = NULL;
			recent_domains->last = NULL;
			recent_domains->length = 0;
			domain_map = initialize_map(0);


			for(line_index=0; line_index < num_lines; line_index++)
			{
				char* line = lines[line_index];
				unsigned long num_pieces;
				char** split = split_on_separators(line, whitespace_chars, 2, -1, 0, &num_pieces);
			
				//check that there are 4 pieces (time, src_ip, dst_ip, domain_name)
				int length;
				for(length=0; split[length] != NULL ; length++){}
				if(length == 3)
				{
					time_t time;
					int parsed_ip[4];
					int valid_ip = sscanf(split[1], "%d.%d.%d.%d", parsed_ip, parsed_ip+1, parsed_ip+2, parsed_ip+3);
					if(valid_ip == 4)
					{
						valid_ip = parsed_ip[0] <= 255 && parsed_ip[1] <= 255 && parsed_ip[2] <= 255 && parsed_ip[3] <= 255 ? valid_ip : 0;
					}
					if(sscanf(split[0], "%ld", &time) > 0 && valid_ip == 4)
					{
						char* domain = split[2];
						char domain_key[700];
						uint32_t ip = parsed_ip[0] + (parsed_ip[1]<<8) + (parsed_ip[2]<<16) +  (parsed_ip[3]<<24) ;

						sprintf(domain_key, STRIP"@%s", IP2STR(ip), domain);
						add_queue_node(ip, domain, recent_domains, domain_map, domain_key, max_domain_queue_length );
						(recent_domains->first->time).tv_sec = time;

					}
				}
				
				for(length=0; split[length] != NULL ; length++)
				{
					free(split[length]);
				}
				free(split);
				free(line);
			}
			free(lines);
		}		
		spin_unlock_bh(&webmon_lock);

	}
		
	
	return 1;
}
static struct nf_sockopt_ops ipt_webmon_sockopts = 
{
	.pf         = PF_INET,
	.set_optmin = WEBMON_SET,
	.set_optmax = WEBMON_SET+1,
	.set        = ipt_webmon_set_ctl,
};



#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,28)
	#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,23)
		static bool 
	#else
		static int
	#endif
	match(		const struct sk_buff *skb,
			const struct net_device *in,
			const struct net_device *out,
			#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
				const struct xt_match *match,
			#endif
			const void *matchinfo,
			int offset,
			#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
				unsigned int protoff,
			#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
				const void *hdr,
				u_int16_t datalen,
			#endif
			#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,23)
				bool *hotdrop
			#else
				int *hotdrop
			#endif	
			)
#else
	static bool match(const struct sk_buff *skb, const struct xt_match_param *par)
#endif
{
	#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,28)
		const struct ipt_webmon_info *info = (const struct ipt_webmon_info*)matchinfo;
	#else
		const struct ipt_webmon_info *info = (const struct ipt_webmon_info*)(par->matchinfo);
	#endif

	
	struct iphdr* iph;

	/* linearize skb if necessary */
	struct sk_buff *linear_skb;
	int skb_copied;
	if(skb_is_nonlinear(skb))
	{
		linear_skb = skb_copy(skb, GFP_ATOMIC);
		skb_copied = 1;
	}
	else
	{
		linear_skb = (struct sk_buff*)skb;
		skb_copied = 0;
	}

	

	/* ignore packets that are not TCP */
	iph = (struct iphdr*)(skb_network_header(skb));
	if(iph->protocol == IPPROTO_TCP)
	{
		/* get payload */
		struct tcphdr* tcp_hdr		= (struct tcphdr*)( ((unsigned char*)iph) + (iph->ihl*4) );
		unsigned short payload_offset 	= (tcp_hdr->doff*4) + (iph->ihl*4);
		unsigned char* payload 		= ((unsigned char*)iph) + payload_offset;
		unsigned short payload_length	= ntohs(iph->tot_len) - payload_offset;

	

		/* if payload length <= 10 bytes don't bother doing a check, otherwise check for match */
		if(payload_length > 10)
		{
			/* are we dealing with a web page request */
			if(strnicmp((char*)payload, "GET ", 4) == 0 || strnicmp(  (char*)payload, "POST ", 5) == 0 || strnicmp((char*)payload, "HEAD ", 5) == 0)
			{
				char domain[650];
				char path[650];
				char domain_key[700];	
				unsigned char save = info->exclude_type == WEBMON_EXCLUDE ? 1 : 0;
				uint32_t ip_index;

			
				for(ip_index = 0; ip_index < info->num_exclude_ips; ip_index++)
				{
					if( (info->exclude_ips)[ip_index] == iph->saddr )
					{
						save = info->exclude_type == WEBMON_EXCLUDE ? 0 : 1;
					}
				}
				for(ip_index=0; ip_index < info->num_exclude_ranges; ip_index++)
				{
					struct ipt_webmon_ip_range r = (info->exclude_ranges)[ip_index];
					if( ntohl( r.start) >= ntohl(iph->saddr) && ntohl(r.end) <= ntohl(iph->saddr) )
					{
						save = info->exclude_type == WEBMON_EXCLUDE ? 0 : 1;
					}
				}


				if(save)
				{
					extract_url(payload, payload_length, domain, path);
					sprintf(domain_key, STRIP"@%s", IP2STR(iph->saddr), domain);

					spin_lock_bh(&webmon_lock);
					if(get_string_map_element(domain_map, domain_key))
					{
						//update time
						update_queue_node_time( (queue_node*)get_map_element(domain_map, domain_key), recent_domains );
					}
					else
					{
						//add
						add_queue_node(iph->saddr, domain, recent_domains, domain_map, domain_key, max_domain_queue_length );
					}
					spin_unlock_bh(&webmon_lock);
				}
			}
		}
	}
	
	/* free skb if we made a copy to linearize it */
	if(skb_copied == 1)
	{
		kfree_skb(linear_skb);
	}


	/* printk("returning %d from webmon\n\n\n", test); */
	return 0;
}



#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,28)
	#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23)
	static bool
	#else
	static int
	#endif
	checkentry(	const char *tablename,
	#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
				const void *ip,
				const struct xt_match *match,
	#else
				const struct ipt_ip *ip,
	#endif
				void *matchinfo,
	#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
		    		unsigned int matchsize,
	#endif
				unsigned int hook_mask
				)
#else
	static bool checkentry(const struct xt_mtchk_param *par)
#endif
{
	#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,28)
		struct ipt_webmon_info *info = (struct ipt_webmon_info*)matchinfo;
	#else
		struct ipt_webmon_info *info = (struct ipt_webmon_info*)(par->matchinfo);
	#endif


	
	if(info->ref_count == NULL) /* first instance, we're inserting rule */
	{
		info->ref_count = (uint32_t*)kmalloc(sizeof(uint32_t), GFP_ATOMIC);
		if(info->ref_count == NULL) /* deal with kmalloc failure */
		{
			printk("ipt_webmon: kmalloc failure in checkentry!\n");
			return 0;
		}
		*(info->ref_count) = 1;

		spin_lock_bh(&webmon_lock);
		
		if(info->max_searches > max_search_queue_length)
		{
			max_search_queue_length = info->max_searches;
		}
		if(info->max_domains > max_domain_queue_length)
		{
			max_domain_queue_length = info->max_domains;
		}
	
		spin_unlock_bh(&webmon_lock);

	}
	else
	{
		*(info->ref_count) = *(info->ref_count) + 1;
	}
	return 1;
}

static void destroy(	
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
			void* matchinfo,
			unsigned int matchinfosize
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,28)
			const struct xt_match *match,
			void* matchinfo
#else
			const struct xt_mtdtor_param *par
#endif
		)
{
	#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,28)
		struct ipt_webmon_info *info = (struct ipt_webmon_info*)matchinfo;
	#else
		struct ipt_webmon_info *info = (struct ipt_webmon_info*)(par->matchinfo);
	#endif

	*(info->ref_count) = *(info->ref_count) - 1;
	if(*(info->ref_count) == 0)
	{
		kfree(info->ref_count);
	}
}

static struct ipt_match webmon_match = 
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	{ NULL, NULL },
	"webmon",
	&match,
	&checkentry,
	&destroy,
	THIS_MODULE
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	.name		= "webmon",
	.match		= &match,
	.family		= AF_INET,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
	.matchsize	= sizeof(struct ipt_webmon_info),
#endif
	.checkentry	= &checkentry,
	.destroy	= &destroy,
	.me		= THIS_MODULE,
#endif
};

static int __init init(void)
{
	spin_lock_bh(&webmon_lock);

	recent_domains = (queue*)malloc(sizeof(queue));
	recent_domains->first = NULL;
	recent_domains->last = NULL;
	recent_domains->length = 0;
	domain_map = initialize_map(0);

	#ifdef CONFIG_PROC_FS
		struct proc_dir_entry *proc_webmon_recent_domains  = create_proc_entry("webmon_recent_domains", 0, NULL);
		if(proc_webmon_recent_domains)
		{
			proc_webmon_recent_domains->proc_fops = &webmon_proc_fops;
		}
	#endif
	
	if (nf_register_sockopt(&ipt_webmon_sockopts) < 0)
	{
		printk("ipt_webmon: Can't register sockopts. Aborting\n");
		spin_unlock_bh(&webmon_lock);
		return -1;
	}
	spin_unlock_bh(&webmon_lock);

	return ipt_register_match(&webmon_match);
}

static void __exit fini(void)
{

	unsigned long num_destroyed;

	spin_lock_bh(&webmon_lock);


	#ifdef CONFIG_PROC_FS
		remove_proc_entry("webmon_recent_domains", NULL);
	#endif
	nf_unregister_sockopt(&ipt_webmon_sockopts);
	ipt_unregister_match(&webmon_match);
	destroy_map(domain_map, DESTROY_MODE_IGNORE_VALUES, &num_destroyed);
	destroy_queue(recent_domains);

	spin_unlock_bh(&webmon_lock);


}

module_init(init);
module_exit(fini);


