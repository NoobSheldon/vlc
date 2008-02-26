/*****************************************************************************
 * audioscrobbler.c : audioscrobbler submission plugin
 *****************************************************************************
 * Copyright © 2006-2008 the VideoLAN team
 * $Id$
 *
 * Author: Rafaël Carré <funman at videolanorg>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/* audioscrobbler protocol version: 1.2
 * http://www.audioscrobbler.net/development/protocol/
 *
 * TODO:    "Now Playing" feature (not mandatory)
 */
/*****************************************************************************
 * Preamble
 *****************************************************************************/

#if defined( WIN32 ) 
#include <time.h> 
#endif 

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc/vlc.h>
#include <vlc_interface.h>
#include <vlc_meta.h>
#include <vlc_md5.h>
#include <vlc_block.h>
#include <vlc_stream.h>
#include <vlc_url.h>
#include <vlc_network.h>
#include <vlc_interface.h>
#include <vlc_playlist.h>

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

#define QUEUE_MAX 50

/* Keeps track of metadata to be submitted */
typedef struct audioscrobbler_song_t
{
    char        *psz_a;             /**< track artist     */
    char        *psz_t;             /**< track title      */
    char        *psz_b;             /**< track album      */
    char        *psz_n;             /**< track number     */
    int         i_l;                /**< track length     */
    char        *psz_m;             /**< musicbrainz id   */
    time_t      date;               /**< date since epoch */
} audioscrobbler_song_t;

struct intf_sys_t
{
    audioscrobbler_song_t   p_queue[QUEUE_MAX]; /**< songs not submitted yet*/
    int                     i_songs;            /**< number of songs        */

    vlc_mutex_t             lock;               /**< p_sys mutex            */

    /* data about audioscrobbler session */
    mtime_t                 next_exchange;      /**< when can we send data  */
    unsigned int            i_interval;         /**< waiting interval (secs)*/

    /* submission of played songs */
    char                    *psz_submit_host;   /**< where to submit data   */
    int                     i_submit_port;      /**< port to which submit   */
    char                    *psz_submit_file;   /**< file to which submit   */

    /* submission of playing song */
#if 0 //NOT USED
    char                    *psz_nowp_host;     /**< where to submit data   */
    int                     i_nowp_port;        /**< port to which submit   */
    char                    *psz_nowp_file;     /**< file to which submit   */
#endif
    vlc_bool_t              b_handshaked;       /**< are we authenticated ? */
    char                    psz_auth_token[33]; /**< Authentication token */

    /* data about song currently playing */
    audioscrobbler_song_t   p_current_song;     /**< song being played      */

    mtime_t                 time_pause;         /**< time when vlc paused   */
    mtime_t                 time_total_pauses;  /**< total time in pause    */

    vlc_bool_t              b_submit;           /**< do we have to submit ? */

    vlc_bool_t              b_state_cb;         /**< if we registered the
                                                 * "state" callback         */

    vlc_bool_t              b_meta_read;        /**< if we read the song's
                                                 * metadata already         */
};

static int  Open            ( vlc_object_t * );
static void Close           ( vlc_object_t * );
static void Unload          ( intf_thread_t * );
static void Run             ( intf_thread_t * );

static int ItemChange       ( vlc_object_t *, const char *, vlc_value_t,
                                vlc_value_t, void * );
static int PlayingChange    ( vlc_object_t *, const char *, vlc_value_t,
                                vlc_value_t, void * );

static void AddToQueue      ( intf_thread_t * );
static int Handshake        ( intf_thread_t * );
static int ReadMetaData     ( intf_thread_t * );
static void DeleteSong      ( audioscrobbler_song_t* );
static int ParseURL         ( char *, char **, char **, int * );
static void HandleInterval  ( mtime_t *, unsigned int * );

/*****************************************************************************
 * Module descriptor
 ****************************************************************************/

#define USERNAME_TEXT       N_("Username")
#define USERNAME_LONGTEXT   N_("The username of your last.fm account")
#define PASSWORD_TEXT       N_("Password")
#define PASSWORD_LONGTEXT   N_("The password of your last.fm account")

/* This error value is used when last.fm plugin has to be unloaded. */
#define VLC_AUDIOSCROBBLER_EFATAL -69

/* last.fm client identifier */
#define CLIENT_NAME     PACKAGE
#define CLIENT_VERSION  VERSION

/* HTTP POST request : to submit data */
#define    POST_REQUEST "POST /%s HTTP/1.1\n"                               \
                        "Accept-Encoding: identity\n"                       \
                        "Content-length: %u\n"                              \
                        "Connection: close\n"                               \
                        "Content-type: application/x-www-form-urlencoded\n" \
                        "Host: %s\n"                                        \
                        "User-agent: VLC Media Player/%s\r\n"               \
                        "\r\n"                                              \
                        "%s\r\n"                                            \
                        "\r\n"

vlc_module_begin();
    set_category( CAT_INTERFACE );
    set_subcategory( SUBCAT_INTERFACE_CONTROL );
    set_shortname( N_( "Audioscrobbler" ) );
    set_description( N_("Submission of played songs to last.fm") );
    add_string( "lastfm-username", "", NULL,
                USERNAME_TEXT, USERNAME_LONGTEXT, VLC_FALSE );
    add_password( "lastfm-password", "", NULL,
                PASSWORD_TEXT, PASSWORD_LONGTEXT, VLC_FALSE );
    set_capability( "interface", 0 );
    set_callbacks( Open, Close );
vlc_module_end();

/*****************************************************************************
 * Open: initialize and create stuff
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    playlist_t      *p_playlist;
    intf_thread_t   *p_intf     = ( intf_thread_t* ) p_this;
    intf_sys_t      *p_sys      = calloc( 1, sizeof( intf_sys_t ) );

    if( !p_sys )
        return VLC_ENOMEM;

    p_intf->p_sys = p_sys;

    vlc_mutex_init( p_this, &p_sys->lock );

    p_playlist = pl_Yield( p_intf );
    PL_LOCK;
    var_AddCallback( p_playlist, "playlist-current", ItemChange, p_intf );
    PL_UNLOCK;
    pl_Release( p_playlist );

    p_intf->pf_run = Run;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: destroy interface stuff
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    playlist_t                  *p_playlist;
    input_thread_t              *p_input;
    intf_thread_t               *p_intf = ( intf_thread_t* ) p_this;
    intf_sys_t                  *p_sys  = p_intf->p_sys;

    p_playlist = pl_Yield( p_intf );
    PL_LOCK;

    var_DelCallback( p_playlist, "playlist-current", ItemChange, p_intf );

    p_input = p_playlist->p_input;
    if ( p_input )
    {
        vlc_object_yield( p_input );

        if( p_sys->b_state_cb )
            var_DelCallback( p_input, "state", PlayingChange, p_intf );

        vlc_object_release( p_input );
    }

    PL_UNLOCK;
    pl_Release( p_playlist );

    p_intf->b_dead = VLC_TRUE;
    /* we lock the mutex in case p_sys is being accessed from a callback */
    vlc_mutex_lock ( &p_sys->lock );
    int i;
    for( i = 0; i < p_sys->i_songs; i++ )
        DeleteSong( &p_sys->p_queue[i] );
    free( p_sys->psz_submit_host );
    free( p_sys->psz_submit_file );
#if 0 //NOT USED
    free( p_sys->psz_nowp_host );
    free( p_sys->psz_nowp_file );
#endif
    vlc_mutex_unlock ( &p_sys->lock );
    vlc_mutex_destroy( &p_sys->lock );
    free( p_sys );
}


/*****************************************************************************
 * Unload: Unloads the audioscrobbler when encountering fatal errors
 *****************************************************************************/
static void Unload( intf_thread_t *p_this )
{
    vlc_object_kill( p_this );
    vlc_object_detach( p_this );
    if( p_this->p_module )
        module_Unneed( p_this, p_this->p_module );
    vlc_mutex_destroy( &p_this->change_lock );
    vlc_object_release( p_this );
}

/*****************************************************************************
 * Run : call Handshake() then submit songs
 *****************************************************************************/
static void Run( intf_thread_t *p_intf )
{
    char                    *psz_submit, *psz_submit_song, *psz_submit_tmp;
    int                     i_net_ret;
    int                     i_song;
    uint8_t                 p_buffer[1024];
    char                    *p_buffer_pos;
    int                     i_post_socket;

    intf_sys_t *p_sys = p_intf->p_sys;

    /* main loop */
    for( ;; )
    {
        vlc_bool_t b_die = VLC_FALSE, b_wait = VLC_FALSE;

        vlc_object_lock( p_intf );
        if( vlc_object_alive( p_intf ) )
        {
           if( mdate() < p_sys->next_exchange )
                /* wait until we can resubmit, i.e.  */
                b_wait = !vlc_object_timedwait( p_intf,
                                                p_sys->next_exchange );
            else
                /* wait for data to submit */
                /* we are signaled each time there is a song to submit */
               vlc_object_wait( p_intf );
        }
        b_die = !vlc_object_alive( p_intf );
        vlc_object_unlock( p_intf );

        if( b_die )
        {
            msg_Dbg( p_intf, "audioscrobbler is dying");
            return;
        }
        if( b_wait )
            continue; /* holding on until next_exchange */

        /* handshake if needed */
        if( p_sys->b_handshaked == VLC_FALSE )
        {
            msg_Dbg( p_intf, "Handshaking with last.fm ..." );

            switch( Handshake( p_intf ) )
            {
                case VLC_ENOMEM:
                    Unload( p_intf );
                    return;

                case VLC_ENOVAR:
                    /* username not set */
                    intf_UserFatal( p_intf, VLC_FALSE,
                        _("Last.fm username not set"),
                        _("Please set a username or disable the "
                        "audioscrobbler plugin, and restart VLC.\n"
                        "Visit http://www.last.fm/join/ to get an account.")
                    );
                    Unload( p_intf );
                    return;

                case VLC_SUCCESS:
                    msg_Dbg( p_intf, "Handshake successfull :)" );
                    p_sys->b_handshaked = VLC_TRUE;
                    p_sys->i_interval = 0;
                    p_sys->next_exchange = mdate();
                    break;

                case VLC_AUDIOSCROBBLER_EFATAL:
                    msg_Warn( p_intf, "Unloading..." );
                    Unload( p_intf );
                    return;

                case VLC_EGENERIC:
                default:
                    /* protocol error : we'll try later */
                    HandleInterval( &p_sys->next_exchange, &p_sys->i_interval );
                    break;
            }
            /* if handshake failed let's restart the loop */
            if( p_sys->b_handshaked == VLC_FALSE )
                continue;
        }

        msg_Dbg( p_intf, "Going to submit some data..." );

        if( !asprintf( &psz_submit, "s=%s", p_sys->psz_auth_token ) )
        {   /* Out of memory */
            Unload( p_intf );
            return;
        }

        /* forge the HTTP POST request */
        vlc_mutex_lock( &p_sys->lock );
        audioscrobbler_song_t *p_song;
        for( i_song = 0 ; i_song < p_sys->i_songs ; i_song++ )
        {
            p_song = &p_sys->p_queue[i_song];
            if( !asprintf( &psz_submit_song,
                    "&a%%5B%d%%5D=%s&t%%5B%d%%5D=%s"
                    "&i%%5B%d%%5D=%llu&o%%5B%d%%5D=P&r%%5B%d%%5D="
                    "&l%%5B%d%%5D=%d&b%%5B%d%%5D=%s"
                    "&n%%5B%d%%5D=%s&m%%5B%d%%5D=%s",
                    i_song, p_song->psz_a,           i_song, p_song->psz_t,
                    i_song, (uintmax_t)p_song->date, i_song, i_song,
                    i_song, p_song->i_l,             i_song, p_song->psz_b,
                    i_song, p_song->psz_n,           i_song, p_song->psz_m
            ) )
            {   /* Out of memory */
                vlc_mutex_unlock( &p_sys->lock );
                Unload( p_intf );
                return;
            }
            psz_submit_tmp = psz_submit;
            if( !asprintf( &psz_submit, "%s%s",
                    psz_submit_tmp, psz_submit_song ) )
            {   /* Out of memory */
                free( psz_submit_tmp );
                free( psz_submit_song );
                vlc_mutex_unlock( &p_sys->lock );
                Unload( p_intf );
                return;
            }
            free( psz_submit_song );
            free( psz_submit_tmp );
        }
        vlc_mutex_unlock( &p_sys->lock );

        i_post_socket = net_ConnectTCP( p_intf,
            p_sys->psz_submit_host, p_sys->i_submit_port );

        if ( i_post_socket == -1 )
        {
            /* If connection fails, we assume we must handshake again */
            HandleInterval( &p_sys->next_exchange, &p_sys->i_interval );
            p_sys->b_handshaked = VLC_FALSE;
            free( psz_submit );
            continue;
        }

        /* we transmit the data */
        i_net_ret = net_Printf(
            VLC_OBJECT( p_intf ), i_post_socket, NULL,
            POST_REQUEST, p_sys->psz_submit_file,
            (unsigned)strlen( psz_submit ), p_sys->psz_submit_file,
            VERSION, psz_submit
        );

        free( psz_submit );
        if ( i_net_ret == -1 )
        {
            /* If connection fails, we assume we must handshake again */
            HandleInterval( &p_sys->next_exchange, &p_sys->i_interval );
            p_sys->b_handshaked = VLC_FALSE;
            continue;
        }

        i_net_ret = net_Read( p_intf, i_post_socket, NULL,
                    p_buffer, 1023, VLC_FALSE );
        if ( i_net_ret <= 0 )
        {
            /* if we get no answer, something went wrong : try again */
            continue;
        }

        net_Close( i_post_socket );
        p_buffer[i_net_ret] = '\0';

        p_buffer_pos = strstr( ( char * ) p_buffer, "FAILED" );
        if ( p_buffer_pos )
        {
            msg_Warn( p_intf, "%s", p_buffer_pos );
            HandleInterval( &p_sys->next_exchange, &p_sys->i_interval );
            continue;
        }

        p_buffer_pos = strstr( ( char * ) p_buffer, "BADSESSION" );
        if ( p_buffer_pos )
        {
            msg_Err( p_intf, "Authentication failed (BADSESSION), are you connected to last.fm with another program ?" );
            p_sys->b_handshaked = VLC_FALSE;
            HandleInterval( &p_sys->next_exchange, &p_sys->i_interval );
            continue;
        }

        p_buffer_pos = strstr( ( char * ) p_buffer, "OK" );
        if ( p_buffer_pos )
        {
            int i;
            for( i = 0; i < p_sys->i_songs; i++ )
                DeleteSong( &p_sys->p_queue[i] );
            p_sys->i_songs = 0;
            p_sys->i_interval = 0;
            p_sys->next_exchange = mdate();
            msg_Dbg( p_intf, "Submission successful!" );
        }
        else
        {
            msg_Err( p_intf, "Authentication failed, handshaking again (%s)", 
                             p_buffer );
            p_sys->b_handshaked = VLC_FALSE;
            HandleInterval( &p_sys->next_exchange, &p_sys->i_interval );
            continue;
        }
    }
}

/*****************************************************************************
 * PlayingChange: Playing status change callback
 *****************************************************************************/
static int PlayingChange( vlc_object_t *p_this, const char *psz_var,
                       vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    intf_thread_t   *p_intf = ( intf_thread_t* ) p_data;
    intf_sys_t      *p_sys  = p_intf->p_sys;

    VLC_UNUSED( p_this ); VLC_UNUSED( psz_var );

    if( p_intf->b_dead )
        return VLC_SUCCESS;

    if( p_sys->b_meta_read == VLC_FALSE && newval.i_int >= PLAYING_S )
    {
        ReadMetaData( p_intf );
        return VLC_SUCCESS;
    }

    if( newval.i_int >= END_S )
        AddToQueue( p_intf );
    else if( oldval.i_int == PLAYING_S && newval.i_int == PAUSE_S )
        p_sys->time_pause = mdate();
    else if( oldval.i_int == PAUSE_S && newval.i_int == PLAYING_S )
        p_sys->time_total_pauses += ( mdate() - p_sys->time_pause );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * ItemChange: Playlist item change callback
 *****************************************************************************/
static int ItemChange( vlc_object_t *p_this, const char *psz_var,
                       vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    playlist_t          *p_playlist;
    input_thread_t      *p_input;
    intf_thread_t       *p_intf     = ( intf_thread_t* ) p_data;
    intf_sys_t          *p_sys      = p_intf->p_sys;
    input_item_t        *p_item;
    vlc_value_t         video_val;

    VLC_UNUSED( p_this ); VLC_UNUSED( psz_var );
    VLC_UNUSED( oldval ); VLC_UNUSED( newval );

    if( p_intf->b_dead )
        return VLC_SUCCESS;

    p_sys->b_state_cb       = VLC_FALSE;
    p_sys->b_meta_read      = VLC_FALSE;
    p_sys->b_submit         = VLC_FALSE;

    p_playlist = pl_Yield( p_intf );
    PL_LOCK;
    p_input = p_playlist->p_input;

    if( !p_input || p_input->b_dead )
    {
        PL_UNLOCK;
        pl_Release( p_playlist );
        return VLC_SUCCESS;
    }

    vlc_object_yield( p_input );
    PL_UNLOCK;
    pl_Release( p_playlist );

    p_item = input_GetItem( p_input );
    if( !p_item )
    {
        vlc_object_release( p_input );
        return VLC_SUCCESS;
    }

    var_Change( p_input, "video-es", VLC_VAR_CHOICESCOUNT, &video_val, NULL );
    if( ( video_val.i_int > 0 ) || p_item->i_type == ITEM_TYPE_NET )
    {
        msg_Dbg( p_this, "Not an audio local file, not submitting");
        vlc_object_release( p_input );
        return VLC_SUCCESS;
    }

    p_sys->time_total_pauses = 0;
    time( &p_sys->p_current_song.date );

    var_AddCallback( p_input, "state", PlayingChange, p_intf );
    p_sys->b_state_cb = VLC_TRUE;

    if( input_item_IsPreparsed( p_item ) )
        ReadMetaData( p_intf );
    /* if the input item was not preparsed, we'll do it in PlayingChange()
     * callback, when "state" == PLAYING_S */

    vlc_object_release( p_input );
    return VLC_SUCCESS;
}

/*****************************************************************************
 * AddToQueue: Add the played song to the queue to be submitted
 *****************************************************************************/
static void AddToQueue ( intf_thread_t *p_this )
{
    mtime_t                     played_time;
    intf_sys_t                  *p_sys = p_this->p_sys;

    vlc_mutex_lock( &p_sys->lock );
    if( !p_sys->b_submit )
        goto end;

    /* wait for the user to listen enough before submitting */
    played_time = mdate();
    played_time -= p_sys->p_current_song.date;
    played_time -= p_sys->time_total_pauses;
    played_time /= 1000000; /* µs → s */

    if( ( played_time < 240 ) &&
        ( played_time < ( p_sys->p_current_song.i_l / 2 ) ) )
    {
        msg_Dbg( p_this, "Song not listened long enough, not submitting" );
        goto end;
    }

    if( p_sys->p_current_song.i_l < 30 )
    {
        msg_Dbg( p_this, "Song too short (< 30s), not submitting" );
        goto end;
    }

    if( !p_sys->p_current_song.psz_a || !*p_sys->p_current_song.psz_a ||
        !p_sys->p_current_song.psz_t || !*p_sys->p_current_song.psz_t )
    {
        msg_Dbg( p_this, "Missing artist or title, not submitting" );
/*XXX*/        msg_Dbg( p_this, "%s %s", p_sys->p_current_song.psz_a, p_sys->p_current_song.psz_t );
        goto end;
    }

    if( p_sys->i_songs >= QUEUE_MAX )
    {
        msg_Warn( p_this, "Submission queue is full, not submitting" );
        goto end;
    }

    msg_Dbg( p_this, "Song will be submitted." );

#define QUEUE_COPY( a ) \
    p_sys->p_queue[p_sys->i_songs].a = p_sys->p_current_song.a

#define QUEUE_COPY_NULL( a ) \
    QUEUE_COPY( a ); \
    p_sys->p_current_song.a = NULL

    QUEUE_COPY( i_l );
    QUEUE_COPY_NULL( psz_n );
    QUEUE_COPY_NULL( psz_a );
    QUEUE_COPY_NULL( psz_t );
    QUEUE_COPY_NULL( psz_b );
    QUEUE_COPY_NULL( psz_m );
    QUEUE_COPY( date );
#undef QUEUE_COPY_NULL
#undef QUEUE_COPY

    p_sys->i_songs++;

    /* signal the main loop we have something to submit */
    vlc_object_signal( VLC_OBJECT( p_this ) );

end:
    DeleteSong( &p_sys->p_current_song );
    p_sys->b_submit = VLC_FALSE;
    vlc_mutex_unlock( &p_sys->lock );
}

/*****************************************************************************
 * ParseURL : Split an http:// URL into host, file, and port
 *
 * Example: "62.216.251.205:80/protocol_1.2"
 *      will be split into "62.216.251.205", 80, "protocol_1.2"
 *
 * psz_url will be freed before returning
 * *psz_file & *psz_host will be freed before use
 *
 * Return value:
 *  VLC_ENOMEM      Out Of Memory
 *  VLC_EGENERIC    Invalid url provided
 *  VLC_SUCCESS     Success
 *****************************************************************************/
static int ParseURL( char *psz_url, char **psz_host, char **psz_file,
                        int *i_port )
{
    int i_pos;
    int i_len = strlen( psz_url );
    FREENULL( *psz_host );
    FREENULL( *psz_file );

    i_pos = strcspn( psz_url, ":" );
    if( i_pos == i_len )
        return VLC_EGENERIC;

    *psz_host = strndup( psz_url, i_pos );
    if( !*psz_host )
        return VLC_ENOMEM;

    i_pos++; /* skip the ':' */
    *i_port = atoi( psz_url + i_pos );
    if( *i_port <= 0 )
    {
        FREENULL( *psz_host );
        return VLC_EGENERIC;
    }

    i_pos = strcspn( psz_url, "/" );

    if( i_pos == i_len )
        return VLC_EGENERIC;

    i_pos++; /* skip the '/' */
    *psz_file = strdup( psz_url + i_pos );
    if( !*psz_file )
    {
        FREENULL( *psz_host );
        return VLC_ENOMEM;
    }

    free( psz_url );
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Handshake : Init audioscrobbler connection
 *****************************************************************************/
static int Handshake( intf_thread_t *p_this )
{
    char                *psz_username, *psz_password;
    time_t              timestamp;
    char                psz_timestamp[33];

    struct md5_s        p_struct_md5;

    stream_t            *p_stream;
    char                *psz_handshake_url;
    uint8_t             p_buffer[1024];
    char                *p_buffer_pos;

    int                 i_ret;
    char                *psz_url;

    intf_thread_t       *p_intf                 = ( intf_thread_t* ) p_this;
    intf_sys_t          *p_sys                  = p_this->p_sys;

    psz_username = config_GetPsz( p_this, "lastfm-username" );
    if( !psz_username )
        return VLC_ENOMEM;

    psz_password = config_GetPsz( p_this, "lastfm-password" );
    if( !psz_password )
    {
        free( psz_username );
        return VLC_ENOMEM;
    }

    /* username or password have not been setup */
    if ( !*psz_username || !*psz_password )
    {
        free( psz_username );
        free( psz_password );
        return VLC_ENOVAR;
    }

    time( &timestamp );

    /* generates a md5 hash of the password */
    InitMD5( &p_struct_md5 );
    AddMD5( &p_struct_md5, ( uint8_t* ) psz_password, strlen( psz_password ) );
    EndMD5( &p_struct_md5 );

    free( psz_password );

    char *psz_password_md5 = psz_md5_hash( &p_struct_md5 );
    if( !psz_password_md5 )
    {
        free( psz_username );
        return VLC_ENOMEM;
    }

    snprintf( psz_timestamp, 33, "%llu", (uintmax_t)timestamp );

    /* generates a md5 hash of :
     * - md5 hash of the password, plus
     * - timestamp in clear text
     */
    InitMD5( &p_struct_md5 );
    AddMD5( &p_struct_md5, ( uint8_t* ) psz_password_md5, 32 );
    AddMD5( &p_struct_md5, ( uint8_t* ) psz_timestamp, strlen( psz_timestamp ));
    EndMD5( &p_struct_md5 );
    free( psz_password_md5 );

    char *psz_auth_token = psz_md5_hash( &p_struct_md5 );
    if( !psz_auth_token )
    {
        free( psz_username );
        return VLC_ENOMEM;
    }
    strncpy( p_sys->psz_auth_token, psz_auth_token, 33 );
    free( psz_auth_token );

    if( !asprintf( &psz_handshake_url,
    "http://post.audioscrobbler.com/?hs=true&p=1.2&c=%s&v=%s&u=%s&t=%s&a=%s",
        CLIENT_NAME, CLIENT_VERSION, psz_username, psz_timestamp,
        p_sys->psz_auth_token ) )
    {
        free( psz_username );
        return VLC_ENOMEM;
    }
    free( psz_username );

    /* send the http handshake request */
    p_stream = stream_UrlNew( p_intf, psz_handshake_url );
    free( psz_handshake_url );

    if( !p_stream )
        return VLC_EGENERIC;

    /* read answer */
    i_ret = stream_Read( p_stream, p_buffer, 1023 );
    if( i_ret == 0 )
    {
        stream_Delete( p_stream );
        return VLC_EGENERIC;
    }
    p_buffer[i_ret] = '\0';
    stream_Delete( p_stream );

    p_buffer_pos = strstr( ( char* ) p_buffer, "FAILED " );
    if ( p_buffer_pos )
    {
        /* handshake request failed, sorry */
        msg_Err( p_this, "last.fm handshake failed: %s", p_buffer_pos + 7 );
        return VLC_EGENERIC;
    }

    p_buffer_pos = strstr( ( char* ) p_buffer, "BADAUTH" );
    if ( p_buffer_pos )
    {
        /* authentication failed, bad username/password combination */
        intf_UserFatal( p_this, VLC_FALSE,
            _("last.fm: Authentication failed"),
            _("last.fm username or password is incorrect. "
              "Please verify your settings and relaunch VLC." ) );
        return VLC_AUDIOSCROBBLER_EFATAL;
    }

    p_buffer_pos = strstr( ( char* ) p_buffer, "BANNED" );
    if ( p_buffer_pos )
    {
        /* oops, our version of vlc has been banned by last.fm servers */
        msg_Err( p_intf, "This version of VLC has been banned by last.fm. "
                         "You should upgrade VLC, or disable the last.fm plugin." );
        return VLC_AUDIOSCROBBLER_EFATAL;
    }

    p_buffer_pos = strstr( ( char* ) p_buffer, "BADTIME" );
    if ( p_buffer_pos )
    {
        /* The system clock isn't good */
        msg_Err( p_intf, "last.fm handshake failed because your clock is too "
                         "much shifted. Please correct it, and relaunch VLC." );
        return VLC_AUDIOSCROBBLER_EFATAL;
    }

    p_buffer_pos = strstr( ( char* ) p_buffer, "OK" );
    if ( !p_buffer_pos )
        goto proto;

    p_buffer_pos = strstr( p_buffer_pos, "\n" );
    if( !p_buffer_pos || strlen( p_buffer_pos ) < 34 )
        goto proto;
    p_buffer_pos++; /* we skip the '\n' */

    /* save the session ID */
    snprintf( p_sys->psz_auth_token, 33, "%s", p_buffer_pos );

    p_buffer_pos = strstr( p_buffer_pos, "http://" );
    if( !p_buffer_pos || strlen( p_buffer_pos ) == 7 )
        goto proto;

    /* We need to read the nowplaying url */
    p_buffer_pos += 7; /* we skip "http://" */
#if 0 //NOT USED
    psz_url = strndup( p_buffer_pos, strcspn( p_buffer_pos, "\n" ) );
    if( !psz_url )
        goto oom;

    switch( ParseURL( psz_url, &p_sys->psz_nowp_host,
                &p_sys->psz_nowp_file, &p_sys->i_nowp_port ) )
    {
        case VLC_ENOMEM:
            goto oom;
        case VLC_EGENERIC:
            goto proto;
        case VLC_SUCCESS:
        default:
            break;
    }
#endif
    p_buffer_pos = strstr( p_buffer_pos, "http://" );
    if( !p_buffer_pos || strlen( p_buffer_pos ) == 7 )
        goto proto;

    /* We need to read the submission url */
    p_buffer_pos += 7; /* we skip "http://" */
    psz_url = strndup( p_buffer_pos, strcspn( p_buffer_pos, "\n" ) );
    if( !psz_url )
        goto oom;

    switch( ParseURL( psz_url, &p_sys->psz_submit_host,
                &p_sys->psz_submit_file, &p_sys->i_submit_port ) )
    {
        case VLC_ENOMEM:
            goto oom;
        case VLC_EGENERIC:
            goto proto;
        case VLC_SUCCESS:
        default:
            break;
    }

    return VLC_SUCCESS;

oom:
    return VLC_ENOMEM;

proto:
    msg_Err( p_intf, "Handshake: can't recognize server protocol" );
    return VLC_EGENERIC;
}

/*****************************************************************************
 * DeleteSong : Delete the char pointers in a song
 *****************************************************************************/
static void DeleteSong( audioscrobbler_song_t* p_song )
{
    FREENULL( p_song->psz_a );
    FREENULL( p_song->psz_b );
    FREENULL( p_song->psz_t );
    FREENULL( p_song->psz_m );
    FREENULL( p_song->psz_n );
}

/*****************************************************************************
 * ReadMetaData : Read meta data when parsed by vlc
 *****************************************************************************/
static int ReadMetaData( intf_thread_t *p_this )
{
    playlist_t          *p_playlist;
    input_thread_t      *p_input;
    input_item_t        *p_item;

    intf_sys_t          *p_sys = p_this->p_sys;

    p_playlist = pl_Yield( p_this );
    PL_LOCK;
    p_input = p_playlist->p_input;
    if( !p_input )
    {
        PL_UNLOCK;
        pl_Release( p_playlist );
        return( VLC_SUCCESS );
    }

    vlc_object_yield( p_input );
    PL_UNLOCK;
    pl_Release( p_playlist );

    p_item = input_GetItem( p_input );
    if( !p_item )
        return VLC_SUCCESS;

    char *psz_meta;
#define ALLOC_ITEM_META( a, b ) \
    psz_meta = input_item_Get##b( p_item ); \
    if( psz_meta && *psz_meta ) \
    { \
        a = encode_URI_component( psz_meta ); \
        if( !a ) \
        { \
            free( psz_meta ); \
            return VLC_ENOMEM; \
        } \
        free( psz_meta ); \
    }

    vlc_mutex_lock( &p_sys->lock );

    p_sys->b_meta_read = VLC_TRUE;

    ALLOC_ITEM_META( p_sys->p_current_song.psz_a, Artist )
    else
    {
        vlc_mutex_unlock( &p_sys->lock );
        msg_Dbg( p_this, "No artist.." );
        vlc_object_release( p_input );
        free( psz_meta );
        return VLC_EGENERIC;
    }

    ALLOC_ITEM_META( p_sys->p_current_song.psz_t, Title )
    else
    {
        vlc_mutex_unlock( &p_sys->lock );
        msg_Dbg( p_this, "No track name.." );
        vlc_object_release( p_input );
        free( p_sys->p_current_song.psz_a );
        free( psz_meta );
        return VLC_EGENERIC;
    }

    /* Now we have read the mandatory meta data, so we can submit that info */
    p_sys->b_submit = VLC_TRUE;

    ALLOC_ITEM_META( p_sys->p_current_song.psz_b, Album )
    else
        p_sys->p_current_song.psz_b = calloc( 1, 1 );

    ALLOC_ITEM_META( p_sys->p_current_song.psz_m, TrackID )
    else
        p_sys->p_current_song.psz_m = calloc( 1, 1 );

    p_sys->p_current_song.i_l = input_item_GetDuration( p_item ) / 1000000;

    ALLOC_ITEM_META( p_sys->p_current_song.psz_n, TrackNum )
    else
        p_sys->p_current_song.psz_n = calloc( 1, 1 );
#undef ALLOC_ITEM_META

    msg_Dbg( p_this, "Meta data registered" );

    vlc_mutex_unlock( &p_sys->lock );
    vlc_object_release( p_input );
    return VLC_SUCCESS;

}

static void HandleInterval( mtime_t *next, unsigned int *i_interval )
{
    if( *i_interval == 0 )
    {
        /* first interval is 1 minute */
        *i_interval = 1;
    }
    else
    {
        /* else we double the previous interval, up to 120 minutes */
        *i_interval <<= 1;
        if( *i_interval > 120 )
            *i_interval = 120;
    }
    *next = mdate() + ( *i_interval * 1000000 * 60 );
}

