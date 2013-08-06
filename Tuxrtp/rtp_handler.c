/*   handler.c
 *
 *   Rtp handler. It's and interface for Rtp. You will use this as the way to communicate to
 *   Rtp session and vice versa. !Red!
 *
 *
 *   Copyright (C) 2013 Tox project All Rights Reserved.
 *
 *   This file is part of Tox.
 *
 *   Tox is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Tox is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "rtp_handler.h"
#include <assert.h>

int rtp_add_user ( rtp_session_t* _session, IP_Port _dest )
{
    if ( !_session ) {
        return FAILURE;
    }

    rtp_dest_list_t* _new_user;
    ALLOCATOR_LIST_S ( _new_user, rtp_dest_list_t, NULL )
    _session->_last_user->next = _new_user;
    _session->_last_user = _new_user;
    return SUCCESS;
}

int rtp_send_msg ( rtp_session_t* _session, rtp_msg_t* _msg )
{
    if ( !_session ) {
        return FAILURE;
    }

    int _last;
    unsigned long long _total = 0;

    for ( rtp_dest_list_t* _it = _session->_dest_list; _it != NULL; _it = _it->next ) {

        if ( !_msg  || _msg->_data == NULL ) {
            _session->_last_error = "Tried to send empty message";
        } else {
            _last = sendpacket ( _it->_dest, _msg->_data, _msg->_length );

            if ( _last < 0 ) {
                _session->_last_error = strerror ( errno );
            } else {
                /* Set sequ number */
                if ( _session->_sequence_number == _MAX_SEQU_NUM ) {
                    _session->_sequence_number = 0;
                } else {
                    _session->_sequence_number++;
                }


                _session->_packets_sent ++;
                _total += _last;
            }
        }

    }

    DEALLOCATOR_MSG ( _msg ) /* free message */
    _session->_bytes_sent += _total;
    return SUCCESS;
}

rtp_msg_t* rtp_recv_msg ( rtp_session_t* _session )
{
    if ( !_session )
        return NULL;


    int32_t  _bytes;
    IP_Port  _from;
    int status = receivepacket ( &_from, LAST_SOCKET_DATA, &_bytes );

    if ( status == FAILURE )  /* nothing recved */
        return NULL;


    LAST_SOCKET_DATA[_bytes] = '\0';

    _session->_bytes_recv += _bytes;
    _session->_packets_recv ++;

    return rtp_msg_parse ( _session, LAST_SOCKET_DATA, _bytes, &_from );
}

rtp_msg_t* rtp_msg_new ( rtp_session_t* _session, uint8_t* _data, uint32_t _length, IP_Port* _from )
{
    rtp_msg_t* _retu;
    ALLOCATOR_S ( _retu, rtp_msg_t )

    /* Sets header values and copies the extension header in _retu */
    _retu->_header = ( rtp_header_t* ) rtp_build_header ( _session ); /* It allocates memory and all */
    _retu->_ext_header = _session->_ext_header;

    _length += _retu->_header->_length;

    /* Allocate Memory for _retu->_data */
    _retu->_data = ( uint8_t* ) malloc ( sizeof ( uint8_t ) * _length );

    /*
     * Parses header into _retu->_data starting from 0
     * Will need to set this _from to 1 since the 0 byte
     * Is used by the messenger to determine that this is rtp.
     */
    uint16_t _from_pos = rtp_add_header ( _retu->_header, _retu->_data, 0, _length );

    /*
     * Parses the extension header into the message
     * Of course if there is any
     */

    if ( _retu->_ext_header ){

        _length += ( 4 + _retu->_ext_header->_ext_len * 4 ) - 1;
        ADD_ALLOCATOR(_retu->_data, uint8_t, _length )

        _from_pos = rtp_add_extention_header( _retu->_ext_header, _retu->_data, _from_pos - 1, _length );
    }

    /* Appends _data on to _retu->_data */
    memadd ( _retu->_data, _from_pos, _data, _length);

    _retu->_length = _length;

    if ( _from ) {
        _retu->_from.ip = _from->ip;
        _retu->_from.port = _from->port;
        _retu->_from.padding = _from->padding;
    }

    return _retu;
}

rtp_msg_t* rtp_msg_parse ( rtp_session_t* _session, uint8_t* _data, uint32_t _length, IP_Port* _from )
{
    rtp_msg_t* _retu;
    ALLOCATOR_S ( _retu, rtp_msg_t )

    _retu->_header = rtp_extract_header ( _data, 0, _length ); /* It allocates memory and all */

    if ( !_retu->_header )
        return NULL;


    _retu->_length = _length - _retu->_header->_length;

    uint16_t _from_pos = _retu->_header->_length;

    if ( rtp_header_get_flag_CSRC_count ( _retu->_header ) == 1 ) { /* Which means initial msg */
        ADD_ALLOCATE ( _session->_csrc, uint32_t, 1 )
        _session->_cc = 2;
        _session->_csrc[1] = _retu->_header->_csrc[0];
        _retu->_header->_length += 4;
    }

    /*
     * Check Sequence number. If this new msg has lesser number then expected drop it return
     * NULL and add stats _packet_loss into _session. RTP does not specify what you do when the packet is lost.
     * You may for example play previous packet, show black screen etc.
     */

    else if ( _retu->_header->_sequence_number < _session->_last_sequence_number ) {
        if ( _retu->_header->_sequence_number != 0 ) { /* if == 0 then it's okay */
            _session->_packet_loss++;
            _session->_last_sequence_number = _retu->_header->_sequence_number;

            free ( _retu->_header );
            free ( _retu );

            return NULL; /* Yes return NULL ( Drop the packet ) */
        }
    }

    _session->_last_sequence_number = _retu->_header->_sequence_number;

    if ( rtp_header_get_flag_extension(_retu->_header) ){
        _retu->_ext_header = rtp_extract_ext_header(_data, _from_pos - 1, _length);
        _retu->_length -= ( 4 + _retu->_ext_header->_ext_len * 4 ) - 1;
        _from_pos += ( 4 + _retu->_ext_header->_ext_len * 4 ) - 1;
    }

    /* Get the payload */
    _retu->_data = malloc ( sizeof ( uint8_t ) * _retu->_length );
    memcpy_from ( _retu->_data, _from_pos, _data, _length);


    if ( _from ) { /* Remove this is not need */
        _retu->_from.ip = _from->ip;
        _retu->_from.port = _from->port;
        _retu->_from.padding = _from->padding;
    }

    return _retu;
}
