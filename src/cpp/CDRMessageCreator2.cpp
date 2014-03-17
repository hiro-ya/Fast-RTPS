/*************************************************************************
 * Copyright (c) 2013 eProsima. All rights reserved.
 *
 * This copy of FastCdr is licensed to you under the terms described in the
 * EPROSIMARTPS_LIBRARY_LICENSE file included in this distribution.
 *
 *************************************************************************/

/*
 * CDRMessageCreator.cpp
 *
 *  Created on: Feb 19, 2014
 *      Author: Gonzalo Rodriguez Canosa
 *      email:  gonzalorodriguez@eprosima.com
 */

#include "eprosimartps/CDRMessageCreator2.h"
#include "eprosimartps/CDRMessage.h"
#include "eprosimartps/ParameterListCreator.h"

namespace eprosima {
namespace rtps{


#if defined(__LITTLE_ENDIAN__)
	const Endianness_t DEFAULT_ENDIAN = LITTLEEND;
#elif defined (__BIG_ENDIAN__)
	const Endianness_t DEFAULT_ENDIAN = BIGEND;
#endif
};
};

using namespace eprosima::dds;

namespace eprosima {
namespace rtps{


CDRMessageCreator2::CDRMessageCreator2() {
	// TODO Auto-generated constructor stub


}

CDRMessageCreator2::~CDRMessageCreator2() {
	// TODO Auto-generated destructor stub
}


bool CDRMessageCreator2::createHeader(CDRMessage_t*msg, GuidPrefix_t guidPrefix,
		ProtocolVersion_t version,VendorId_t vendorId)
{

	try{
		CDRMessage::addOctet(msg,'R');
		CDRMessage::addOctet(msg,'T');
		CDRMessage::addOctet(msg,'P');
		CDRMessage::addOctet(msg,'S');

		CDRMessage::addOctet(msg,version.major);
		CDRMessage::addOctet(msg,version.minor);

		CDRMessage::addOctet(msg,vendorId[0]);
		CDRMessage::addOctet(msg,vendorId[1]);

		for (uint i = 0;i<12;i++){
			CDRMessage::addOctet(msg,guidPrefix.value[i]);
		}
		msg->length = msg->pos;
	}
	catch(int e)
	{
		RTPSLog::Error << B_RED << "Header creation fails: "<< e << DEF<< endl;
		RTPSLog::printError();
		return false;
	}

	return true;
}

bool CDRMessageCreator2::createSubmessageHeader(CDRMessage_t* msg,
		octet id,octet flags,uint16_t size) {

	try{
		CDRMessage::addOctet(msg,id);
		CDRMessage::addOctet(msg,flags);
		CDRMessage::addUInt16(msg, size);
		msg->length = msg->pos;
	}
	catch(int e){
		RTPSLog::Error << B_RED << "Submessage Header creation fails: "<< e << DEF<< endl;
		RTPSLog::printError();
		return false;
	}

	return true;
}






}; /* namespace rtps */
}; /* namespace eprosima */


#include "submessages/DataMsg2.hpp"
#include "submessages/HeartbeatMsg2.hpp"
#include "submessages/AckNackMsg2.hpp"
#include "submessages/GapMsg2.hpp"
