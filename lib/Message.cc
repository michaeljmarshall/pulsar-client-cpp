/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <pulsar/Message.h>
#include <pulsar/MessageBuilder.h>
#include <pulsar/MessageIdBuilder.h>
#include <pulsar/defines.h>

#include <iostream>

#include "Int64SerDes.h"
#include "KeyValueImpl.h"
#include "MessageImpl.h"
#include "PulsarApi.pb.h"
#include "SharedBuffer.h"

using namespace pulsar;

namespace pulsar {

const static std::string emptyString;
const static MessageId invalidMessageId;

const Message::StringMap& Message::getProperties() const { return impl_->properties(); }

bool Message::hasProperty(const std::string& name) const {
    const StringMap& m = impl_->properties();
    return m.find(name) != m.end();
}

const std::string& Message::getProperty(const std::string& name) const {
    if (hasProperty(name)) {
        const StringMap& m = impl_->properties();
        return m.at(name);
    } else {
        return emptyString;
    }
}

const void* Message::getData() const { return impl_->payload.data(); }

std::size_t Message::getLength() const { return impl_->payload.readableBytes(); }

#if defined(_MSC_VER) && !defined(NDEBUG)
const std::string& Message::getDataAsString() const {
    thread_local std::string value;
    value = std::string{static_cast<const char*>(getData()), getLength()};
    return value;
}
#else
std::string Message::getDataAsString() const { return std::string((const char*)getData(), getLength()); }
#endif

Message::Message() : impl_() {}

Message::Message(MessageImplPtr& impl) : impl_(impl) {}

Message::Message(const MessageId& messageId, proto::MessageMetadata& metadata, SharedBuffer& payload)
    : impl_(std::make_shared<MessageImpl>()) {
    impl_->messageId = messageId;
    impl_->metadata = metadata;
    impl_->payload = payload;
}

Message::Message(const MessageId& messageID, proto::MessageMetadata& metadata, SharedBuffer& payload,
                 proto::SingleMessageMetadata& singleMetadata, const std::shared_ptr<std::string>& topicName)
    : impl_(std::make_shared<MessageImpl>()) {
    impl_->messageId = messageID;
    impl_->metadata = metadata;
    impl_->payload = payload;
    impl_->metadata.mutable_properties()->CopyFrom(singleMetadata.properties());
    impl_->topicName_ = topicName;

    impl_->metadata.clear_properties();
    if (singleMetadata.properties_size() > 0) {
        impl_->metadata.mutable_properties()->Reserve(singleMetadata.properties_size());
        for (int i = 0; i < singleMetadata.properties_size(); i++) {
            auto keyValue = proto::KeyValue().New();
            *keyValue = singleMetadata.properties(i);
            impl_->metadata.mutable_properties()->AddAllocated(keyValue);
        }
    }

    if (singleMetadata.has_partition_key()) {
        impl_->metadata.set_partition_key(singleMetadata.partition_key());
    } else {
        impl_->metadata.clear_partition_key();
    }

    if (singleMetadata.has_ordering_key()) {
        impl_->metadata.set_ordering_key(singleMetadata.ordering_key());
    } else {
        impl_->metadata.clear_ordering_key();
    }

    if (singleMetadata.has_event_time()) {
        impl_->metadata.set_event_time(singleMetadata.event_time());
    } else {
        impl_->metadata.clear_event_time();
    }

    if (singleMetadata.has_sequence_id()) {
        impl_->metadata.set_sequence_id(singleMetadata.sequence_id());
    } else {
        impl_->metadata.clear_sequence_id();
    }
}

const MessageId& Message::getMessageId() const {
    if (!impl_) {
        return invalidMessageId;
    } else {
        return impl_->messageId;
    }
}

void Message::setMessageId(const MessageId& messageID) const {
    if (impl_) {
        impl_->messageId = messageID;
    }
    return;
}

bool Message::hasPartitionKey() const {
    if (impl_) {
        return impl_->hasPartitionKey();
    }
    return false;
}

const std::string& Message::getPartitionKey() const {
    if (!impl_) {
        return emptyString;
    }
    return impl_->getPartitionKey();
}

bool Message::hasOrderingKey() const {
    if (impl_) {
        return impl_->hasOrderingKey();
    }
    return false;
}

const std::string& Message::getOrderingKey() const {
    if (!impl_) {
        return emptyString;
    }
    return impl_->getOrderingKey();
}

const std::string& Message::getTopicName() const {
    if (!impl_) {
        return emptyString;
    }
    return impl_->getTopicName();
}

const int Message::getRedeliveryCount() const {
    if (!impl_) {
        return 0;
    }
    return impl_->getRedeliveryCount();
}

bool Message::hasSchemaVersion() const {
    if (impl_) {
        return impl_->hasSchemaVersion();
    }
    return false;
}

int64_t Message::getLongSchemaVersion() const {
    return (impl_ && impl_->hasSchemaVersion()) ? fromBigEndianBytes(impl_->getSchemaVersion()) : -1L;
}

const std::string& Message::getSchemaVersion() const {
    if (!impl_) {
        return emptyString;
    }
    return impl_->getSchemaVersion();
}

uint64_t Message::getPublishTimestamp() const { return impl_ ? impl_->getPublishTimestamp() : 0ull; }

uint64_t Message::getEventTimestamp() const { return impl_ ? impl_->getEventTimestamp() : 0ull; }

bool Message::operator==(const Message& msg) const { return getMessageId() == msg.getMessageId(); }

KeyValue Message::getKeyValueData() const { return KeyValue(impl_->keyValuePtr); }

PULSAR_PUBLIC std::ostream& operator<<(std::ostream& s, const Message::StringMap& map) {
    // Output at most 10 elements -- appropriate if used for logging.
    s << '{';

    Message::StringMap::const_iterator begin = map.begin();
    Message::StringMap::const_iterator end = map.end();
    for (int i = 0; begin != end && i < 10; ++i, ++begin) {
        if (i > 0) {
            s << ", ";
        }

        s << "'" << begin->first << "':'" << begin->second << "'";
    }

    if (begin != end) {
        s << " ...";
    }

    s << '}';
    return s;
}

PULSAR_PUBLIC std::ostream& operator<<(std::ostream& s, const Message& msg) {
    assert(msg.impl_.get());
    assert(msg.impl_->metadata.has_sequence_id());
    assert(msg.impl_->metadata.has_publish_time());
    s << "Message(prod=" << msg.impl_->metadata.producer_name()
      << ", seq=" << msg.impl_->metadata.sequence_id()
      << ", publish_time=" << msg.impl_->metadata.publish_time() << ", payload_size=" << msg.getLength()
      << ", msg_id=" << msg.getMessageId() << ", props=" << msg.getProperties() << ')';
    return s;
}

}  // namespace pulsar
