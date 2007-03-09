/*
 * $Id$
 *
 * Copyright (C) 2001-2003 FhG Fokus
 * Copyright (C) 2005-2006 Voice Sistem S.R.L.
 *
 * This file is part of openser, a free SIP server.
 *
 * openser is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * openser is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * History:
 * ---------
 *  2003-02-28  scratchpad compatibility abandoned (jiri)
 *  2003-01-29  removed scratchpad (jiri)
 *  2003-03-19  fixed set* len calculation bug & simplified a little the code
 *              (should be a little faster now) (andrei)
 *              replaced all mallocs/frees w/ pkg_malloc/pkg_free (andrei)
 *  2003-04-01  Added support for loose routing in forward (janakj)
 *  2003-04-12  FORCE_RPORT_T added (andrei)
 *  2003-04-22  strip_tail added (jiri)
 *  2003-10-02  added SET_ADV_ADDR_T & SET_ADV_PORT_T (andrei)
 *  2003-10-29  added FORCE_TCP_ALIAS_T (andrei)
 *  2004-11-30  added FORCE_SEND_SOCKET_T (andrei)
 *  2005-11-29  added serialize_branches and next_branches (bogdan)
 *  2006-03-02  MODULE_T action points to a cmd_export_t struct instead to 
 *               a function address - more info is accessible (bogdan)
 *  2006-05-22  forward(_udp,_tcp,_tls) and send(_tcp) merged in forward() and
 *               send() (bogdan)
 *  2006-12-22  functions for script and branch flags added (bogdan)
 */

#include "action.h"
#include "config.h"
#include "error.h"
#include "dprint.h"
#include "proxy.h"
#include "forward.h"
#include "udp_server.h"
#include "route.h"
#include "parser/msg_parser.h"
#include "parser/parse_uri.h"
#include "ut.h"
#include "sr_module.h"
#include "mem/mem.h"
#include "globals.h"
#include "dset.h"
#include "flags.h"
#include "errinfo.h"
#include "serialize.h"
#include "blacklists.h"
#ifdef USE_TCP
#include "tcp_server.h"
#endif

#include "script_var.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>

#ifdef DEBUG_DMALLOC
#include <dmalloc.h>
#endif

int action_flags = 0;
int return_code  = 0;

static int rec_lev=0;

extern err_info_t _oser_err_info;


/* run actions from a route */
/* returns: 0, or 1 on success, <0 on error */
/* (0 if drop or break encountered, 1 if not ) */
static inline int run_actions(struct action* a, struct sip_msg* msg)
{
	int ret;

	rec_lev++;
	if (rec_lev>ROUTE_MAX_REC_LEV){
		LOG(L_ERR, "ERROR:run_action: too many recursive routing "
				"table lookups (%d) giving up!\n", rec_lev);
		ret=E_UNSPEC;
		goto error;
	}

	if (a==0){
		LOG(L_WARN, "WARNING: run_actions: null action list (rec_level=%d)\n", 
			rec_lev);
		ret=1;
		goto error;
	}

	ret=run_action_list(a, msg);

	/* if 'return', reset the flag */
	if(action_flags&ACT_FL_RETURN)
		action_flags &= ~ACT_FL_RETURN;

	rec_lev--;
	return ret;

error:
	rec_lev--;
	return ret;
}

/* run a list of actions */
int run_action_list(struct action* a, struct sip_msg* msg)
{
	int ret=E_UNSPEC;
	struct action* t;
	for (t=a; t!=0; t=t->next){
		ret=do_action(t, msg);
		/* if action returns 0, then stop processing the script */
		if(ret==0)
			action_flags |= ACT_FL_EXIT;
		if(error_rlist!=NULL && !is_route_type(ERROR_ROUTE)
				&& !is_route_type(ONREPLY_ROUTE)
				&& _oser_err_info.eclass!=0)
		{
			DBG("run_action_list: jumping to error route\n");
			set_route_type( ERROR_ROUTE );
			run_actions(error_rlist, msg);
			/* if don't exit, then reset error info */
			if(!(action_flags&ACT_FL_EXIT))
				init_err_info();
		}
		
		if((action_flags&ACT_FL_RETURN) || (action_flags&ACT_FL_EXIT))
			break;
	}
	return ret;
}



int run_top_route(struct action* a, struct sip_msg* msg)
{
	int bk_action_flags;
	int bk_rec_lev;
	int ret;

	bk_action_flags = action_flags;
	bk_rec_lev = rec_lev;

	action_flags = 0;
	rec_lev = 0;
	init_err_info();
	reset_bl_markers();

	resetsflag( (unsigned int)-1 );

	run_actions(a, msg);
	ret = action_flags;

	action_flags = bk_action_flags;
	rec_lev = bk_rec_lev;

	return ret;
}


/* execute assignement operation */
int do_assign(struct sip_msg* msg, struct action* a)
{
	int ret;
	xl_value_t val;
	int_str avp_name;
	int_str avp_val;
	int flags;
	unsigned short name_type;
	xl_spec_p dspec;
	struct action  act;
	char backup;

	ret = -1;
	memset(&val, 0, sizeof(xl_value_t));
	if(a->elem[1].type != NULLV_ST)
	{
		ret = eval_expr((struct expr*)a->elem[1].u.data, msg, &val);
		if(!((val.flags&XL_VAL_STR)||(val.flags&XL_VAL_INT))) {
			LOG(L_ERR, "do_assign: no value in right expression\n");
			goto error;
		}
	}

	dspec = (xl_spec_p)a->elem[0].u.data;
	switch ((unsigned char)a->type){
		case EQ_T:
		case PLUSEQ_T:
		case MINUSEQ_T:
		case DIVEQ_T:
		case MULTEQ_T:
		case MODULOEQ_T:
		case BANDEQ_T:
		case BOREQ_T:
		case BXOREQ_T:
			switch(dspec->type) {
				case XL_AVP:
					if(xl_get_avp_name(msg, dspec, &avp_name, &name_type)!=0)
					{
						LOG(L_ERR, "BUG:avpops:ops_printf: error getting"
								" dst AVP name\n");
						goto error;
					}
					if(a->elem[1].type == NULLV_ST)
					{
						destroy_avps(name_type, avp_name, 0);
						return 1;
					}

					flags = name_type;
					if(val.flags&XL_TYPE_INT)
					{
						avp_val.n = val.ri;
					} else {
						avp_val.s = val.rs;
						flags |= AVP_VAL_STR;
					}

					if (add_avp(flags, avp_name, avp_val)<0)
					{
						LOG(L_ERR, "do_assign: error - cannot add AVP\n");
						goto error;
					}

				break;
				case XL_SCRIPTVAR:
					if(dspec->p.data==0)
					{
						LOG(L_ERR, "do_assign: error - cannot find svar\n");
						goto error;
					}
					if(a->elem[1].type == NULLV_ST)
					{
						avp_val.n = 0;
						set_var_value((script_var_t*)dspec->p.data,&avp_val,0);
						return 1;
					}
					flags = 0;
					if(val.flags&XL_TYPE_INT)
					{
						avp_val.n = val.ri;
					} else {
						avp_val.s = val.rs;
						flags |= VAR_VAL_STR;
					}
					if(set_var_value((script_var_t*)dspec->p.data,
								&avp_val, flags)==NULL)
					{
						LOG(L_ERR,
							"do_assign: error - cannot set svar [%.*s]\n",
							dspec->p.val.len, dspec->p.val.s);
						goto error;
					}
				break;
				case XL_RURI_USERNAME:
					if(a->elem[1].type == NULLV_ST)
					{
						memset(&act, 0, sizeof(act));
						act.type = SET_USER_T;
						act.elem[0].type = STRING_ST;
						act.elem[0].u.string = "";
						if (do_action(&act, msg)<0)
						{
							LOG(L_ERR,
								"do_assign: error - do action failed %d\n",
								act.type);
							goto error;
						}
						return 1;
					}
				case XL_RURI_DOMAIN:
				case XL_RURI:
					if(!(val.flags&XL_VAL_STR))
					{
						LOG(L_ERR,"do_assign: error - str value requred to"
								" set R-URI parts\n");
						goto error;
					}
					memset(&act, 0, sizeof(act));
					act.elem[0].type = STRING_ST;
					act.elem[0].u.string = val.rs.s;
					backup = val.rs.s[val.rs.len];
					val.rs.s[val.rs.len] = '\0';
					if(dspec->type==XL_RURI_USERNAME)
						act.type = SET_USER_T;
					else if(dspec->type==XL_RURI_DOMAIN)
						act.type = SET_HOST_T;
					else
						act.type = SET_URI_T;
					if (do_action(&act, msg)<0)
					{
						LOG(L_ERR,"do_assign: error - do action failed %d\n",
								act.type);
						val.rs.s[val.rs.len] = backup;
						goto error;
					}
					val.rs.s[val.rs.len] = backup;
				break;
				case XL_DSTURI:
					if(a->elem[1].type == NULLV_ST)
					{
						memset(&act, 0, sizeof(act));
						act.type = RESET_DSTURI_T;
						if (do_action(&act, msg)<0)
						{
							LOG(L_ERR,
								"do_assign: error - do action failed %d\n",
								act.type);
							goto error;
						}
						return 1;
					}
					if(!(val.flags&XL_VAL_STR))
					{
						LOG(L_ERR,"do_assign: error - str value requred to"
							" set dst uri\n");
						goto error;
					}
					if(set_dst_uri(msg, &val.rs)!=0)
						goto error;
				break;
				default:
					LOG(L_ERR, "do_assign: error - unknown dst var\n");
					return E_BUG;
			}

			xl_value_destroy(&val);
			return 1;
		default:
			LOG(L_CRIT, "BUG: do_assign: unknown type %d\n", a->type);
	}

	xl_value_destroy(&val);
	return ret;

error:
	xl_value_destroy(&val);
	return -1;
}

/* ret= 0! if action -> end of list(e.g DROP), 
      > 0 to continue processing next actions
   and <0 on error */
int do_action(struct action* a, struct sip_msg* msg)
{
	int ret;
	int v;
	union sockaddr_union* to;
	struct proxy_l* p;
	char* tmp;
	char *new_uri, *end, *crt;
	int len;
	int user;
	struct sip_uri uri, next_hop;
	struct sip_uri *u;
	unsigned short port;
	int cmatch;
	struct action *aitem;
	struct action *adefault;
	xl_spec_t *spec;
	xl_value_t val;

	/* reset the value of error to E_UNSPEC so avoid unknowledgable
	   functions to return with error (status<0) and not setting it
	   leaving there previous error; cache the previous value though
	   for functions which want to process it */
	prev_ser_error=ser_error;
	ser_error=E_UNSPEC;

	ret=E_BUG;
	switch ((unsigned char)a->type){
		case DROP_T:
				action_flags |= ACT_FL_DROP;
		case EXIT_T:
				ret=0;
				action_flags |= ACT_FL_EXIT;
			break;
		case RETURN_T:
				ret=a->elem[0].u.number;
				action_flags |= ACT_FL_RETURN;
			break;
		case FORWARD_T:
			if (a->elem[0].type==NOSUBTYPE){
				/* parse uri and build a proxy */
				if (msg->dst_uri.len) {
					ret = parse_uri(msg->dst_uri.s, msg->dst_uri.len,
						&next_hop);
					u = &next_hop;
				} else {
					ret = parse_sip_msg_uri(msg);
					u = &msg->parsed_uri;
				}
				if (ret<0) {
					LOG(L_ERR, "ERROR: do_action: forward: bad_uri "
								" dropping packet\n");
					break;
				}
				/* create a temporary proxy*/
				p=mk_proxy(&u->host, u->port_no, u->proto,
					(u->type==SIPS_URI_T)?1:0 );
				if (p==0){
					LOG(L_ERR, "ERROR:  bad host name in uri,"
							" dropping packet\n");
					ret=E_BAD_ADDRESS;
					goto error_fwd_uri;
				}
				ret=forward_request(msg, p);
				free_proxy(p); /* frees only p content, not p itself */
				pkg_free(p);
				if (ret>=0) ret=1;
			}else if ((a->elem[0].type==PROXY_ST)) {
				ret=forward_request(msg,(struct proxy_l*)a->elem[0].u.data);
				if (ret>=0) ret=1;
			}else{
				LOG(L_CRIT, "BUG: do_action: bad forward() types %d, %d\n",
						a->elem[0].type, a->elem[1].type);
				ret=E_BUG;
			}
			break;
		case SEND_T:
			if (a->elem[0].type!= PROXY_ST){
				LOG(L_CRIT,"BUG: do_action: bad send() type %d\n",
						a->elem[0].type);
				ret=E_BUG;
				break;
			}
			to=(union sockaddr_union*)
					pkg_malloc(sizeof(union sockaddr_union));
			if (to==0){
				LOG(L_ERR, "ERROR: do_action: "
							"memory allocation failure\n");
				ret=E_OUT_OF_MEM;
				break;
			}
			
			p=(struct proxy_l*)a->elem[0].u.data;
			
			ret=hostent2su(to, &p->host, p->addr_idx,
						(p->port)?p->port:SIP_PORT );
			if (ret==0){
				ret = msg_send(0/*send_sock*/, p->proto, to, 0/*id*/,
						msg->buf, msg->len);
				if (ret!=0 && p->host.h_addr_list[p->addr_idx+1])
					p->addr_idx++;
			}
			pkg_free(to);
			if (ret>=0)
				ret=1;
			break;
		case LOG_T:
			if ((a->elem[0].type!=NUMBER_ST)|(a->elem[1].type!=STRING_ST)){
				LOG(L_CRIT, "BUG: do_action: bad log() types %d, %d\n",
						a->elem[0].type, a->elem[1].type);
				ret=E_BUG;
				break;
			}
			LOG(a->elem[0].u.number, a->elem[1].u.string);
			ret=1;
			break;
		case APPEND_BRANCH_T:
			/* WARNING: even if type is STRING_ST, it expects a str !!!*/
			if ((a->elem[0].type!=STRING_ST)) {
				LOG(L_CRIT, "BUG: do_action: bad append_branch_t %d\n",
					a->elem[0].type );
				ret=E_BUG;
				break;
			}
			if (a->elem[0].u.s.s==NULL) {
				ret = append_branch(msg, 0, &msg->dst_uri, 0,
					a->elem[1].u.number, getb0flags(), msg->force_send_socket);
				/* reset all branch info */
				msg->force_send_socket = 0;
				setb0flags(0);
				if(msg->dst_uri.s!=0)
					pkg_free(msg->dst_uri.s);
				msg->dst_uri.s = 0;
				msg->dst_uri.len = 0;
			} else {
				ret = append_branch(msg, &a->elem[0].u.s, &msg->dst_uri, 0,
					a->elem[1].u.number, getb0flags(), msg->force_send_socket);
			}
			break;
		case LEN_GT_T:
			if (a->elem[0].type!=NUMBER_ST) {
				LOG(L_CRIT, "BUG: do_action: bad len_gt type %d\n",
					a->elem[0].type );
				ret=E_BUG;
				break;
			}
			ret = (msg->len >= (unsigned int)a->elem[0].u.number) ? 1 : -1;
			break;
		case SETFLAG_T:
			ret = setflag( msg, a->elem[0].u.number );
			break;
		case RESETFLAG_T:
			ret = resetflag( msg, a->elem[0].u.number );
			break;
		case ISFLAGSET_T:
			ret = isflagset( msg, a->elem[0].u.number );
			break;
		case SETSFLAG_T:
			ret = setsflag( a->elem[0].u.number );
			break;
		case RESETSFLAG_T:
			ret = resetsflag( a->elem[0].u.number );
			break;
		case ISSFLAGSET_T:
			ret = issflagset( a->elem[0].u.number );
			break;
		case SETBFLAG_T:
			ret = setbflag( a->elem[0].u.number, a->elem[1].u.number );
			break;
		case RESETBFLAG_T:
			ret = resetbflag( a->elem[0].u.number, a->elem[1].u.number  );
			break;
		case ISBFLAGSET_T:
			ret = isbflagset( a->elem[0].u.number, a->elem[1].u.number  );
			break;
		case ERROR_T:
			if ((a->elem[0].type!=STRING_ST)|(a->elem[1].type!=STRING_ST)){
				LOG(L_CRIT, "BUG: do_action: bad error() types %d, %d\n",
						a->elem[0].type, a->elem[1].type);
				ret=E_BUG;
				break;
			}
			LOG(L_NOTICE, "WARNING: do_action: error(\"%s\", \"%s\") "
					"not implemented yet\n", a->elem[0].u.string,
					a->elem[1].u.string);
			ret=1;
			break;
		case ROUTE_T:
			if (a->elem[0].type!=NUMBER_ST){
				LOG(L_CRIT, "BUG: do_action: bad route() type %d\n",
						a->elem[0].type);
				ret=E_BUG;
				break;
			}
			if ((a->elem[0].u.number>RT_NO)||(a->elem[0].u.number<0)){
				LOG(L_ERR, "ERROR: invalid routing table number in"
							"route(%lu)\n", a->elem[0].u.number);
				ret=E_CFG;
				break;
			}
			return_code=run_actions(rlist[a->elem[0].u.number], msg);
			ret=(return_code<0)?return_code:1;
			break;
		case REVERT_URI_T:
			if (msg->new_uri.s) {
				pkg_free(msg->new_uri.s);
				msg->new_uri.len=0;
				msg->new_uri.s=0;
				msg->parsed_uri_ok=0; /* invalidate current parsed uri*/
			};
			ret=1;
			break;
		case SET_HOST_T:
		case SET_HOSTPORT_T:
		case SET_USER_T:
		case SET_USERPASS_T:
		case SET_PORT_T:
		case SET_URI_T:
		case PREFIX_T:
		case STRIP_T:
		case STRIP_TAIL_T:
				user=0;
				if (a->type==STRIP_T || a->type==STRIP_TAIL_T) {
					if (a->elem[0].type!=NUMBER_ST) {
						LOG(L_CRIT, "BUG: do_action: bad set*() type %d\n",
							a->elem[0].type);
						break;
					}
				} else if (a->elem[0].type!=STRING_ST){
					LOG(L_CRIT, "BUG: do_action: bad set*() type %d\n",
							a->elem[0].type);
					ret=E_BUG;
					break;
				}
				if (a->type==SET_URI_T){
					if (msg->new_uri.s) {
						pkg_free(msg->new_uri.s);
						msg->new_uri.len=0;
					}
					msg->parsed_uri_ok=0;
					len=strlen(a->elem[0].u.string);
					msg->new_uri.s=pkg_malloc(len+1);
					if (msg->new_uri.s==0){
						LOG(L_ERR, "ERROR: do_action: memory allocation"
								" failure\n");
						ret=E_OUT_OF_MEM;
						break;
					}
					memcpy(msg->new_uri.s, a->elem[0].u.string, len);
					msg->new_uri.s[len]=0;
					msg->new_uri.len=len;
					
					ret=1;
					break;
				}
				if (msg->new_uri.s) {
					tmp=msg->new_uri.s;
					len=msg->new_uri.len;
				}else{
					tmp=msg->first_line.u.request.uri.s;
					len=msg->first_line.u.request.uri.len;
				}
				if (parse_uri(tmp, len, &uri)<0){
					LOG(L_ERR, "ERROR: do_action: bad uri <%s>, dropping"
								" packet\n", tmp);
					ret=E_UNSPEC;
					break;
				}
				
				new_uri=pkg_malloc(MAX_URI_SIZE);
				if (new_uri==0){
					LOG(L_ERR, "ERROR: do_action: memory allocation failure\n");
					ret=E_OUT_OF_MEM;
					break;
				}
				end=new_uri+MAX_URI_SIZE;
				crt=new_uri;
				/* begin copying */
				len=strlen("sip:"); if(crt+len>end) goto error_uri;
				memcpy(crt,"sip:",len);crt+=len;

				if (a->type==PREFIX_T) {
					tmp=a->elem[0].u.string;
					len=strlen(tmp); if(crt+len>end) goto error_uri;
					memcpy(crt,tmp,len);crt+=len;
					/* whatever we had before, with prefix we have username 
					   now */
					user=1;
				}

				if ((a->type==SET_USER_T)||(a->type==SET_USERPASS_T)) {
					tmp=a->elem[0].u.string;
					len=strlen(tmp);
				} else if (a->type==STRIP_T) {
					if (a->elem[0].u.number>uri.user.len) {
						LOG(L_WARN, "Error: too long strip asked; "
								" deleting username: %lu of <%.*s>\n",
								a->elem[0].u.number, uri.user.len, uri.user.s);
						len=0;
					} else if (a->elem[0].u.number==uri.user.len) {
						len=0;
					} else {
						tmp=uri.user.s + a->elem[0].u.number;
						len=uri.user.len - a->elem[0].u.number;
					}
				} else if (a->type==STRIP_TAIL_T) {
					if (a->elem[0].u.number>uri.user.len) {
						LOG(L_WARN, "WARNING: too long strip_tail asked;"
								" deleting username: %lu of <%.*s>\n",
								a->elem[0].u.number, uri.user.len, uri.user.s);
						len=0;
					} else if (a->elem[0].u.number==uri.user.len) {
						len=0;
					} else {
						tmp=uri.user.s;
						len=uri.user.len - a->elem[0].u.number;
					}
				} else {
					tmp=uri.user.s;
					len=uri.user.len;
				}

				if (len){
					if(crt+len>end) goto error_uri;
					memcpy(crt,tmp,len);crt+=len;
					user=1; /* we have an user field so mark it */
				}

				if (a->type==SET_USERPASS_T) tmp=0;
				else tmp=uri.passwd.s;
				/* passwd */
				if (tmp){
					len=uri.passwd.len; if(crt+len+1>end) goto error_uri;
					*crt=':'; crt++;
					memcpy(crt,tmp,len);crt+=len;
				}
				/* host */
				if (user || tmp){ /* add @ */
					if(crt+1>end) goto error_uri;
					*crt='@'; crt++;
				}
				if ((a->type==SET_HOST_T) ||(a->type==SET_HOSTPORT_T)) {
					tmp=a->elem[0].u.string;
					if (tmp) len = strlen(tmp);
					else len=0;
				} else {
					tmp=uri.host.s;
					len = uri.host.len;
				}
				if (tmp){
					if(crt+len>end) goto error_uri;
					memcpy(crt,tmp,len);crt+=len;
				}
				/* port */
				if (a->type==SET_HOSTPORT_T) tmp=0;
				else if (a->type==SET_PORT_T) {
					tmp=a->elem[0].u.string;
					if (tmp) len = strlen(tmp);
					else len = 0;
				} else {
					tmp=uri.port.s;
					len = uri.port.len;
				}
				if (tmp){
					if(crt+len+1>end) goto error_uri;
					*crt=':'; crt++;
					memcpy(crt,tmp,len);crt+=len;
				}
				/* params */
				tmp=uri.params.s;
				if (tmp){
					len=uri.params.len; if(crt+len+1>end) goto error_uri;
					*crt=';'; crt++;
					memcpy(crt,tmp,len);crt+=len;
				}
				/* headers */
				tmp=uri.headers.s;
				if (tmp){
					len=uri.headers.len; if(crt+len+1>end) goto error_uri;
					*crt='?'; crt++;
					memcpy(crt,tmp,len);crt+=len;
				}
				*crt=0; /* null terminate the thing */
				/* copy it to the msg */
				if (msg->new_uri.s) pkg_free(msg->new_uri.s);
				msg->new_uri.s=new_uri;
				msg->new_uri.len=crt-new_uri;
				msg->parsed_uri_ok=0;
				ret=1;
				break;
		case SET_DSTURI_T:
			/* WARNING: even if type is STRING_ST, it expects a str !!!*/
			if (a->elem[0].type!=STRING_ST){
				LOG(L_CRIT, "BUG: do_action: bad setdsturi() type %d\n",
							a->elem[0].type);
				ret=E_BUG;
				break;
			}
			if(set_dst_uri(msg, &a->elem[0].u.s)!=0)
				ret = -1;
			else
				ret = 1;
			break;
		case RESET_DSTURI_T:
			if(msg->dst_uri.s!=0)
				pkg_free(msg->dst_uri.s);
			msg->dst_uri.s = 0;
			msg->dst_uri.len = 0;
			ret = 1;
			break;
		case ISDSTURISET_T:
			if(msg->dst_uri.s==0 || msg->dst_uri.len<=0)
				ret = -1;
			else
				ret = 1;
			break;
		case IF_T:
				/* if null expr => ignore if? */
				if ((a->elem[0].type==EXPR_ST)&&a->elem[0].u.data){
					v=eval_expr((struct expr*)a->elem[0].u.data, msg, 0);
					/* set return code to expr value */
					if (v<0 || (action_flags&ACT_FL_RETURN)
							|| (action_flags&ACT_FL_EXIT) ){
						if (v==EXPR_DROP || (action_flags&ACT_FL_RETURN)
								|| (action_flags&ACT_FL_EXIT) ){ /* hack to quit on DROP*/
							ret=0;
							return_code = 0;
							break;
						}else{
							LOG(L_WARN,"WARNING: do_action:"
										"error in expression\n");
						}
					}
					
					ret=1;  /*default is continue */
					if (v>0) {
						if ((a->elem[1].type==ACTIONS_ST)&&a->elem[1].u.data){
							ret=run_action_list(
									(struct action*)a->elem[1].u.data,msg );
							return_code = ret;
						} else return_code = v;
					}else{
						if ((a->elem[2].type==ACTIONS_ST)&&a->elem[2].u.data){
							ret=run_action_list(
								(struct action*)a->elem[2].u.data,msg);
							return_code = ret;
						} else return_code = v;
					}
				}
			break;
		case SWITCH_T:
			if (a->elem[0].type!=SCRIPTVAR_ST){
				LOG(L_CRIT, "BUG: do_action: bad switch() type %d\n",
						a->elem[0].type);
				ret=E_BUG;
				break;
			}
			spec = (xl_spec_t*)a->elem[0].u.data;
			if(xl_get_spec_value(msg, spec, &val, 0)!=0)
			{
				LOG(L_ERR, "BUG: do_action: no value in switch()\n");
				ret=E_BUG;
				break;
			}
			/* get the value of pvar */
			if(a->elem[1].type!=ACTIONS_ST) {
				LOG(L_CRIT, "BUG: do_action: bad switch() actions\n");
				ret=E_BUG;
				break;
			}
			return_code=1;
			adefault = NULL;
			aitem = (struct action*)a->elem[1].u.data;
			cmatch=0;
			while(aitem)
			{
				if((unsigned char)aitem->type==DEFAULT_T)
					adefault=aitem;
				if(cmatch==0)
				{
					if(aitem->elem[0].type==STRING_ST)
					{
						if(val.flags&XL_VAL_STR
								&& val.rs.len==aitem->elem[0].u.s.len
								&& strncasecmp(val.rs.s, aitem->elem[0].u.s.s,
									val.rs.len)==0)
							cmatch = 1;
					} else { /* number */
						if(val.flags&XL_VAL_INT && 
								val.ri==aitem->elem[0].u.number)
							cmatch = 1;
					}
				}
				if(cmatch==1)
				{
					if(aitem->elem[1].u.data)
					{
						return_code=run_action_list(
							(struct action*)aitem->elem[1].u.data, msg);
						if ((action_flags&ACT_FL_RETURN) ||
						(action_flags&ACT_FL_EXIT))
							break;
					}
					if(aitem->elem[2].u.number==1)
						break;
				}
				aitem = aitem->next;
			}
			if((cmatch==0) && (adefault!=NULL))
			{
				DBG("do_action: swtich: running default statement\n");
				if(adefault->elem[0].u.data)
					return_code=run_action_list(
						(struct action*)adefault->elem[0].u.data, msg);
			}
			ret=(return_code<0)?return_code:1;
			break;
		case MODULE_T:
			if ( (a->elem[0].type==CMD_ST) && a->elem[0].u.data ) {
				ret=((cmd_export_t*)(a->elem[0].u.data))->function(msg,
						(char*)a->elem[1].u.data, (char*)a->elem[2].u.data);
			}else{
				LOG(L_CRIT,"BUG: do_action: bad module call\n");
			}
			break;
		case FORCE_RPORT_T:
			msg->msg_flags|=FL_FORCE_RPORT;
			ret=1; /* continue processing */
			break;
		case FORCE_LOCAL_RPORT_T:
			msg->msg_flags|=FL_FORCE_LOCAL_RPORT;
			ret=1; /* continue processing */
			break;
		case SET_ADV_ADDR_T:
			if (a->elem[0].type!=STR_ST){
				LOG(L_CRIT, "BUG: do_action: bad set_advertised_address() "
						"type %d\n", a->elem[0].type);
				ret=E_BUG;
				break;
			}
			msg->set_global_address=*((str*)a->elem[0].u.data);
			ret=1; /* continue processing */
			break;
		case SET_ADV_PORT_T:
			if (a->elem[0].type!=STR_ST){
				LOG(L_CRIT, "BUG: do_action: bad set_advertised_port() "
						"type %d\n", a->elem[0].type);
				ret=E_BUG;
				break;
			}
			msg->set_global_port=*((str*)a->elem[0].u.data);
			ret=1; /* continue processing */
			break;
#ifdef USE_TCP
		case FORCE_TCP_ALIAS_T:
			if ( msg->rcv.proto==PROTO_TCP
#ifdef USE_TLS
					|| msg->rcv.proto==PROTO_TLS
#endif
			   ){
				
				if (a->elem[0].type==NOSUBTYPE)	port=msg->via1->port;
				else if (a->elem[0].type==NUMBER_ST)
					port=(int)a->elem[0].u.number;
				else{
					LOG(L_CRIT, "BUG: do_action: bad force_tcp_alias"
							" port type %d\n", a->elem[0].type);
					ret=E_BUG;
					break;
				}
						
				if (tcpconn_add_alias(msg->rcv.proto_reserved1, port,
									msg->rcv.proto)!=0){
					LOG(L_ERR, " ERROR:do_action: tcp alias failed\n");
					ret=E_UNSPEC;
					break;
				}
			}
#endif
			ret=1; /* continue processing */
			break;
		case FORCE_SEND_SOCKET_T:
			if (a->elem[0].type!=SOCKETINFO_ST){
				LOG(L_CRIT, "BUG: do_action: bad force_send_socket argument"
						" type: %d\n", a->elem[0].type);
				ret=E_BUG;
				break;
			}
			msg->force_send_socket=(struct socket_info*)a->elem[0].u.data;
			ret=1; /* continue processing */
			break;
		case SERIALIZE_BRANCHES_T:
			if (a->elem[0].type!=NUMBER_ST){
				LOG(L_CRIT, "BUG: do_action: bad serialize_branches argument"
						" type: %d\n", a->elem[0].type);
				ret=E_BUG;
				break;
			}
			if (serialize_branches(msg,(int)a->elem[0].u.number)!=0) {
				LOG(L_ERR, "ERROR: do_action: serialize_branches failed\n");
				ret=E_UNSPEC;
				break;
			}
			ret=1; /* continue processing */
			break;
		case NEXT_BRANCHES_T:
			if (next_branches(msg)!=0) {
				LOG(L_ERR, "ERROR: do_action: next_branches failed\n");
				ret=E_UNSPEC;
				break;
			}
			ret=1; /* continue processing */
			break;
		case EQ_T:
		case PLUSEQ_T:
		case MINUSEQ_T:
		case DIVEQ_T:
		case MULTEQ_T:
		case MODULOEQ_T:
		case BANDEQ_T:
		case BOREQ_T:
		case BXOREQ_T:
			ret = do_assign(msg, a);
			break;
		case USE_BLACKLIST_T:
			mark_for_search((struct bl_head*)a->elem[0].u.data);
			break;
#ifdef TIMING_INFO
		case SET_TIME_STAMP_T:
				set_time_stamp(a->elem[0].u.string);
			break;
		case RESET_TIME_STAMP_T:
				reset_time_stamp();
			break;
		case DIFF_TIME_STAMP_T:
				diff_time_stamp(a->elem[0].u.number, a->elem[1].u.string);
			break;
#endif
		default:
			LOG(L_CRIT, "BUG: do_action: unknown type %d\n", a->type);
	}

	if((unsigned char)a->type!=IF_T && (unsigned char)a->type!=ROUTE_T)
		return_code = ret;
/*skip:*/
	return ret;
	
error_uri:
	LOG(L_ERR, "ERROR: do_action: set*: uri too long\n");
	if (new_uri) pkg_free(new_uri);
	return E_UNSPEC;
error_fwd_uri:
	return ret;
}



