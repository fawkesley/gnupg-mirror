/* keygen.c - generate a key pair
 *	Copyright (c) 1997 by Werner Koch (dd9jn)
 *
 * This file is part of G10.
 *
 * G10 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * G10 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "util.h"
#include "main.h"
#include "packet.h"
#include "cipher.h"
#include "ttyio.h"
#include "options.h"

#if 0
  #define TEST_ALGO  1
  #define TEST_NBITS 256
  #define TEST_UID   "Karl Test"
#endif


static int
answer_is_yes( const char *s )
{
    if( !stricmp(s, "yes") )
	return 1;
    if( *s == 'y' && !s[1] )
	return 1;
    if( *s == 'Y' && !s[1] )
	return 1;
    return 0;
}


static u16
checksum_u16( unsigned n )
{
    u16 a;

    a  = (n >> 8) & 0xff;
    a |= n & 0xff;
    return a;
}

static u16
checksum( byte *p, unsigned n )
{
    u16 a;

    for(a=0; n; n-- )
	a += *p++;
    return a;
}

static u16
checksum_mpi( MPI a )
{
    u16 csum;
    byte *buffer;
    unsigned nbytes;

    buffer = mpi_get_buffer( a, &nbytes, NULL );
    csum = checksum_u16( nbytes*8 );
    csum += checksum( buffer, nbytes );
    m_free( buffer );
    return csum;
}



static void
write_uid( IOBUF out, const char *s, PKT_user_id **upkt )
{
    PACKET pkt;
    size_t n = strlen(s);
    int rc;

    pkt.pkttype = PKT_USER_ID;
    pkt.pkt.user_id = m_alloc( sizeof *pkt.pkt.user_id + n - 1 );
    pkt.pkt.user_id->len = n;
    strcpy(pkt.pkt.user_id->name, s);
    if( (rc = build_packet( out, &pkt )) )
	log_error("build_packet(user_id) failed: %s\n", g10_errstr(rc) );
    if( upkt ) {
	*upkt = pkt.pkt.user_id;
	pkt.pkt.user_id = NULL;
    }
    free_packet( &pkt );
}


static int
write_selfsig( IOBUF out, PKT_public_cert *pkc, PKT_user_id *uid,
						PKT_secret_cert *skc )
{
    PACKET pkt;
    PKT_signature *sig;
    int rc=0;

    if( opt.verbose )
	log_info("writing self signature\n");

    rc = make_keysig_packet( &sig, pkc, uid, skc, 0x13, DIGEST_ALGO_RMD160 );
    if( rc ) {
	log_error("make_keysig_packet failed: %s\n", g10_errstr(rc) );
	return rc;
    }

    pkt.pkttype = PKT_SIGNATURE;
    pkt.pkt.signature = sig;
    if( (rc = build_packet( out, &pkt )) )
	log_error("build_packet(signature) failed: %s\n", g10_errstr(rc) );
    free_packet( &pkt );
    return rc;
}


#ifdef HAVE_RSA_CIPHER
static int
gen_rsa(unsigned nbits, IOBUF pub_io, IOBUF sec_io, DEK *dek,
	PKT_public_cert **ret_pkc, PKT_secret_cert **ret_skc )
{
    int rc;
    PACKET pkt1, pkt2;
    PKT_secret_cert *skc;
    PKT_public_cert *pkc;
    RSA_public_key pk;
    RSA_secret_key sk;

    init_packet(&pkt1);
    init_packet(&pkt2);

    rsa_generate( &pk, &sk, nbits );

    skc = m_alloc( sizeof *skc );
    pkc = m_alloc( sizeof *pkc );
    skc->timestamp = pkc->timestamp = make_timestamp();
    skc->valid_days = pkc->valid_days = 0; /* fixme: make it configurable*/
    skc->pubkey_algo = pkc->pubkey_algo = PUBKEY_ALGO_RSA;
		       memset(&pkc->mfx, 0, sizeof pkc->mfx);
		       pkc->d.rsa.rsa_n = pk.n;
		       pkc->d.rsa.rsa_e = pk.e;
    skc->d.rsa.rsa_n = sk.n;
    skc->d.rsa.rsa_e = sk.e;
    skc->d.rsa.rsa_d = sk.d;
    skc->d.rsa.rsa_p = sk.p;
    skc->d.rsa.rsa_q = sk.q;
    skc->d.rsa.rsa_u = sk.u;
    skc->d.rsa.csum  = checksum_mpi( skc->d.rsa.rsa_d );
    skc->d.rsa.csum += checksum_mpi( skc->d.rsa.rsa_p );
    skc->d.rsa.csum += checksum_mpi( skc->d.rsa.rsa_q );
    skc->d.rsa.csum += checksum_mpi( skc->d.rsa.rsa_u );
    if( !dek ) {
	skc->d.rsa.is_protected = 0;
	skc->d.rsa.protect_algo = 0;
    }
    else {
	skc->d.rsa.is_protected = 1;
	skc->d.rsa.protect_algo = CIPHER_ALGO_BLOWFISH;
	randomize_buffer( skc->d.rsa.protect.blowfish.iv, 8, 1);
	skc->d.rsa.csum += checksum( skc->d.rsa.protect.blowfish.iv, 8 );
	rc = protect_secret_key( skc, dek );
	if( rc ) {
	    log_error("protect_secret_key failed: %s\n", g10_errstr(rc) );
	    goto leave;
	}
    }

    pkt1.pkttype = PKT_PUBLIC_CERT;
    pkt1.pkt.public_cert = pkc;
    pkt2.pkttype = PKT_SECRET_CERT;
    pkt2.pkt.secret_cert = skc;

    if( (rc = build_packet( pub_io, &pkt1 )) ) {
	log_error("build public_cert packet failed: %s\n", g10_errstr(rc) );
	goto leave;
    }
    if( (rc = build_packet( sec_io, &pkt2 )) ) {
	log_error("build secret_cert packet failed: %s\n", g10_errstr(rc) );
	goto leave;
    }
    *ret_pkc = pkt1.pkt.public_cert;
    pkt1.pkt.public_cert = NULL;
    *ret_skc = pkt1.pkt.secret_cert;
    pkt1.pkt.secret_cert = NULL;

  leave:
    free_packet(&pkt1);
    free_packet(&pkt2);
    return rc;
}
#endif /*HAVE_RSA_CIPHER*/

static int
gen_elg(unsigned nbits, IOBUF pub_io, IOBUF sec_io, DEK *dek,
	PKT_public_cert **ret_pkc, PKT_secret_cert **ret_skc )
{
    int rc;
    PACKET pkt1, pkt2;
    PKT_secret_cert *skc, *unprotected_skc;
    PKT_public_cert *pkc;
    ELG_public_key pk;
    ELG_secret_key sk;
    unsigned nbytes;

    init_packet(&pkt1);
    init_packet(&pkt2);

    elg_generate( &pk, &sk, nbits );

    skc = m_alloc( sizeof *skc );
    pkc = m_alloc( sizeof *pkc );
    skc->timestamp = pkc->timestamp = make_timestamp();
    skc->valid_days = pkc->valid_days = 0; /* fixme: make it configurable*/
    skc->pubkey_algo = pkc->pubkey_algo = PUBKEY_ALGO_ELGAMAL;
		       memset(&pkc->mfx, 0, sizeof pkc->mfx);
		       pkc->d.elg.p = pk.p;
		       pkc->d.elg.g = pk.g;
		       pkc->d.elg.y = pk.y;
    skc->d.elg.p = sk.p;
    skc->d.elg.g = sk.g;
    skc->d.elg.y = sk.y;
    skc->d.elg.x = sk.x;

    skc->d.elg.csum = checksum_mpi( skc->d.elg.x );
    unprotected_skc = copy_secret_cert( NULL, skc );
    if( !dek ) {
	skc->d.elg.is_protected = 0;
	skc->d.elg.protect_algo = 0;
    }
    else {
	skc->d.elg.is_protected = 0;
	skc->d.elg.protect_algo = CIPHER_ALGO_BLOWFISH;
	randomize_buffer(skc->d.elg.protect.blowfish.iv, 8, 1);
	rc = protect_secret_key( skc, dek );
	if( rc ) {
	    log_error("protect_secret_key failed: %s\n", g10_errstr(rc) );
	    goto leave;
	}
    }

    pkt1.pkttype = PKT_PUBLIC_CERT;
    pkt1.pkt.public_cert = pkc;
    pkt2.pkttype = PKT_SECRET_CERT;
    pkt2.pkt.secret_cert = skc;

    if( (rc = build_packet( pub_io, &pkt1 )) ) {
	log_error("build public_cert packet failed: %s\n", g10_errstr(rc) );
	goto leave;
    }
    if( (rc = build_packet( sec_io, &pkt2 )) ) {
	log_error("build secret_cert packet failed: %s\n", g10_errstr(rc) );
	goto leave;
    }
    *ret_pkc = pkt1.pkt.public_cert;
    pkt1.pkt.public_cert = NULL;
    *ret_skc = unprotected_skc;
    unprotected_skc = NULL;


  leave:
    free_packet(&pkt1);
    free_packet(&pkt2);
    if( unprotected_skc )
	free_secret_cert( unprotected_skc );
    return rc;
}



/****************
 * Generate a keypair
 */
void
generate_keypair()
{
    char *answer;
    unsigned nbits;
    char *pub_fname = "./pubring.g10";
    char *sec_fname = "./secring.g10";
    char *uid = NULL;
    IOBUF pub_io = NULL;
    IOBUF sec_io = NULL;
    PKT_public_cert *pkc = NULL;
    PKT_secret_cert *skc = NULL;
    PKT_user_id *upkt = NULL;
    DEK *dek = NULL;
    int rc;
    int algo;
    const char *algo_name;

  #ifndef TEST_ALGO
    if( opt.batch || opt.answer_yes || opt.answer_no )
	log_fatal("Key generation can only be used in interactive mode\n");

    tty_printf("Please select the algorithm to use:\n"
	       "   (1) ElGamal is the suggested one.\n"
	   #ifdef HAVE_RSA_CIPHER
	       "   (2) RSA cannot be used inthe U.S.\n"
	   #endif
	       );
  #endif

    for(;;) {
      #ifdef TEST_ALGO
	algo = TEST_ALGO;
      #else
	answer = tty_get("Your selection? (1,2) ");
	tty_kill_prompt();
	algo = *answer? atoi(answer): 1;
	m_free(answer);
      #endif
	if( algo == 1 ) {
	    algo = PUBKEY_ALGO_ELGAMAL;
	    algo_name = "ElGamal";
	    break;
	}
      #ifdef HAVE_RSA_CIPHER
	else if( algo == 2 ) {
	    algo = PUBKEY_ALGO_RSA;
	    algo_name = "RSA";
	    break;
	}
      #endif
    }



    tty_printf("About to generate a new %s keypair.\n"
	  #ifndef TEST_NBITS
	       "              minimum keysize is  768 bits\n"
	       "              default keysize is 1024 bits\n"
	       "    highest suggested keysize is 2048 bits\n"
	  #endif
							     , algo_name );
    for(;;) {
      #ifdef TEST_NBITS
	nbits = TEST_NBITS;
      #else
	answer = tty_get("What keysize do you want? (1024) ");
	tty_kill_prompt();
	nbits = *answer? atoi(answer): 1024;
	m_free(answer);
      #endif
	if( nbits < 128 ) /* FIXME: change this to 768 */
	    tty_printf("keysize too small; please select a larger one\n");
	else if( nbits > 2048 ) {
	    tty_printf("Keysizes larger than 2048 are not suggested, because "
		       "computations take REALLY long!\n");
	    answer = tty_get("Are you sure, that you want this keysize? ");
	    tty_kill_prompt();
	    if( answer_is_yes(answer) ) {
		m_free(answer);
		tty_printf("Okay, but keep in mind that your monitor "
			   "and keyboard radiation is also very vulnerable "
			   "to attacks!\n");
		break;
	    }
	    m_free(answer);
	}
	else
	    break;
    }
    tty_printf("Requested keysize is %u bits\n", nbits );
    if( (nbits % 32) ) {
	nbits = ((nbits + 31) / 32) * 32;
	tty_printf("rounded up to %u bits\n", nbits );
    }

  #ifdef TEST_UID
    uid = m_alloc(strlen(TEST_UID)+1);
    strcpy(uid, TEST_UID);
  #else
    tty_printf( "\nYou need a User-ID to identify your key; please use your name and your\n"
		"email address in this suggested format:\n"
		"    \"Heinrich Heine <heinrichh@uni-duesseldorf.de>\n" );
    uid = NULL;
    for(;;) {
	m_free(uid);
	tty_printf("\n");
	uid = tty_get("Your User-ID: ");
	tty_kill_prompt();
	if( strlen(uid) < 5 )
	    tty_printf("Please enter a string of at least 5 characters\n");
	else  {
	    tty_printf("You selected this USER-ID:\n    \"%s\"\n\n", uid);
	    answer = tty_get("Is this correct? ");
	    tty_kill_prompt();
	    if( answer_is_yes(answer) ) {
		m_free(answer);
		break;
	    }
	    m_free(answer);
	}
    }
  #endif


    tty_printf( "You need a Passphrase to protect your secret key.\n\n" );

    dek = m_alloc_secure( sizeof *dek );
    dek->algo = CIPHER_ALGO_BLOWFISH;
    rc = make_dek_from_passphrase( dek , 2 );
    if( rc == -1 ) {
	m_free(dek); dek = NULL;
	tty_printf(
	    "You don't what a passphrase - this is probably a *bad* idea!\n"
	    "I will do it anyway.  You can change your passphrase at anytime,\n"
	    "using this program with the option \"--change-passphrase\"\n\n" );
    }
    else if( rc ) {
	m_free(dek); dek = NULL;
	m_free(uid);
	log_error("Error getting the passphrase: %s\n", g10_errstr(rc) );
	return;
    }


    /* now check wether we a are allowed to write the keyrings */
    if( !(rc=overwrite_filep( pub_fname )) ) {
	if( !(pub_io = iobuf_create( pub_fname )) )
	    log_error("can't create %s: %s\n", pub_fname, strerror(errno) );
	else if( opt.verbose )
	    log_info("writing to '%s'\n", pub_fname );
    }
    else if( rc != -1 ) {
	log_error("Oops: overwrite_filep(%s): %s\n", pub_fname, g10_errstr(rc) );
	m_free(uid);
	return;
    }
    else {
	m_free(uid);
	return;
    }
    if( !(rc=overwrite_filep( sec_fname )) ) {
	if( !(sec_io = iobuf_create( sec_fname )) )
	    log_error("can't create %s: %s\n", sec_fname, strerror(errno) );
	else if( opt.verbose )
	    log_info("writing to '%s'\n", sec_fname );
    }
    else if( rc != -1 ) {
	log_error("Oops: overwrite_filep(%s): %s\n", sec_fname, g10_errstr(rc) );
	m_free(uid);
	return;
    }
    else {
	iobuf_cancel(pub_io);
	m_free(uid);
	return;
    }

    write_comment( pub_io, "#public key created by G10 pre-release " VERSION );
    write_comment( sec_io, "#secret key created by G10 pre-release " VERSION );

    if( algo == PUBKEY_ALGO_ELGAMAL )
	rc = gen_elg(nbits, pub_io, sec_io, dek, &pkc, &skc);
  #ifdef HAVE_RSA_CIPHER
    else if( algo == PUBKEY_ALGO_RSA )
	rc = gen_rsa(nbits, pub_io, sec_io, dek, &pkc, &skc);
  #endif
    else
	log_bug(NULL);
    if( !rc )
	write_uid(pub_io, uid, &upkt );
    if( !rc )
	write_uid(sec_io, uid, NULL );
    if( !rc )
	rc = write_selfsig(pub_io, pkc, upkt, skc );

    if( rc ) {
	iobuf_cancel(pub_io);
	iobuf_cancel(sec_io);
	tty_printf("Key generation failed: %s\n", g10_errstr(rc) );
    }
    else {
	iobuf_close(pub_io);
	iobuf_close(sec_io);
	tty_printf("public and secret key created and signed.\n" );
    }
    if( pkc )
	free_public_cert( pkc );
    if( skc )
	free_secret_cert( skc );
    if( upkt )
	free_user_id( upkt );
    m_free(uid);
    m_free(dek);
}

