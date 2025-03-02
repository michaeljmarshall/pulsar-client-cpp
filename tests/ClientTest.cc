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
#include <gtest/gtest.h>
#include <pulsar/Client.h>
#include <pulsar/Version.h>

#include <algorithm>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <chrono>
#include <future>
#include <sstream>

#include "HttpHelper.h"
#include "PulsarFriend.h"
#include "WaitUtils.h"
#include "lib/ClientConnection.h"
#include "lib/LogUtils.h"
#include "lib/checksum/ChecksumProvider.h"
#include "lib/stats/ProducerStatsImpl.h"

DECLARE_LOG_OBJECT()

using namespace pulsar;

static std::string lookupUrl = "pulsar://localhost:6650";
static std::string adminUrl = "http://localhost:8080/";

TEST(ClientTest, testChecksumComputation) {
    std::string data = "test";
    std::string doubleData = "testtest";

    // (1) compute checksum of specific chunk of string
    int checksum1 = computeChecksum(0, (char *)data.c_str(), data.length());
    int checksum2 = computeChecksum(0, (char *)doubleData.c_str() + 4, 4);
    ASSERT_EQ(checksum1, checksum2);

    //(2) compute incremental checksum
    // (a) checksum on full data
    int doubleChecksum = computeChecksum(0, (char *)doubleData.c_str(), doubleData.length());
    // (b) incremental checksum on multiple partial data
    checksum1 = computeChecksum(0, (char *)data.c_str(), data.length());
    int incrementalChecksum = computeChecksum(checksum1, (char *)data.c_str(), data.length());
    ASSERT_EQ(incrementalChecksum, doubleChecksum);
}

TEST(ClientTest, testSwHwChecksum) {
    std::string data = "test";
    std::string doubleData = "testtest";

    // (1) compute checksum of specific chunk of string
    // (a) HW
    uint32_t hwChecksum1 = crc32cHw(0, (char *)data.c_str(), data.length());
    uint32_t hwChecksum2 = crc32cHw(0, (char *)doubleData.c_str() + 4, 4);
    // (b) SW
    uint32_t swChecksum1 = crc32cSw(0, (char *)data.c_str(), data.length());
    uint32_t swChecksum2 = crc32cSw(0, (char *)doubleData.c_str() + 4, 4);

    ASSERT_EQ(hwChecksum1, hwChecksum2);
    ASSERT_EQ(hwChecksum1, swChecksum1);
    ASSERT_EQ(hwChecksum2, swChecksum2);

    //(2) compute incremental checksum
    // (a.1) hw: checksum on full data
    uint32_t hwDoubleChecksum = crc32cHw(0, (char *)doubleData.c_str(), doubleData.length());
    // (a.2) hw: incremental checksum on multiple partial data
    hwChecksum1 = crc32cHw(0, (char *)data.c_str(), data.length());
    uint32_t hwIncrementalChecksum = crc32cHw(hwChecksum1, (char *)data.c_str(), data.length());
    // (b.1) sw: checksum on full data
    uint32_t swDoubleChecksum = crc32cSw(0, (char *)doubleData.c_str(), doubleData.length());
    ASSERT_EQ(swDoubleChecksum, hwDoubleChecksum);
    // (b.2) sw: incremental checksum on multiple partial data
    swChecksum1 = crc32cHw(0, (char *)data.c_str(), data.length());
    uint32_t swIncrementalChecksum = crc32cSw(swChecksum1, (char *)data.c_str(), data.length());
    ASSERT_EQ(hwIncrementalChecksum, hwDoubleChecksum);
    ASSERT_EQ(hwIncrementalChecksum, swIncrementalChecksum);
    ASSERT_EQ(hwIncrementalChecksum, swIncrementalChecksum);
}

TEST(ClientTest, testServerConnectError) {
    const std::string topic = "test-server-connect-error";
    Client client("pulsar://localhost:65535", ClientConfiguration().setOperationTimeoutSeconds(1));
    Producer producer;
    ASSERT_EQ(ResultTimeout, client.createProducer(topic, producer));
    Consumer consumer;
    ASSERT_EQ(ResultTimeout, client.subscribe(topic, "sub", consumer));
    Reader reader;
    ReaderConfiguration readerConf;
    ASSERT_EQ(ResultTimeout, client.createReader(topic, MessageId::earliest(), readerConf, reader));
    client.close();
}

TEST(ClientTest, testConnectTimeout) {
    // 192.0.2.0/24 is assigned for documentation, should be a deadend
    const std::string blackHoleBroker = "pulsar://192.0.2.1:1234";
    const std::string topic = "test-connect-timeout";

    Client clientLow(blackHoleBroker, ClientConfiguration().setConnectionTimeout(1000));
    Client clientDefault(blackHoleBroker);

    std::promise<Result> promiseLow;
    clientLow.createProducerAsync(
        topic, [&promiseLow](Result result, Producer producer) { promiseLow.set_value(result); });

    std::promise<Result> promiseDefault;
    clientDefault.createProducerAsync(
        topic, [&promiseDefault](Result result, Producer producer) { promiseDefault.set_value(result); });

    auto futureLow = promiseLow.get_future();
    ASSERT_EQ(futureLow.wait_for(std::chrono::milliseconds(1500)), std::future_status::ready);
    ASSERT_EQ(futureLow.get(), ResultConnectError);

    auto futureDefault = promiseDefault.get_future();
    ASSERT_EQ(futureDefault.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);

    clientLow.close();
    clientDefault.close();

    ASSERT_EQ(futureDefault.wait_for(std::chrono::milliseconds(10)), std::future_status::ready);
    ASSERT_EQ(futureDefault.get(), ResultDisconnected);
}

TEST(ClientTest, testGetNumberOfReferences) {
    Client client("pulsar://localhost:6650");

    // Producer test
    uint64_t numberOfProducers = 0;
    const std::string nonPartitionedTopic =
        "testGetNumberOfReferencesNonPartitionedTopic" + std::to_string(time(nullptr));

    const std::string partitionedTopic =
        "testGetNumberOfReferencesPartitionedTopic" + std::to_string(time(nullptr));
    Producer producer;
    client.createProducer(nonPartitionedTopic, producer);
    numberOfProducers = 1;
    ASSERT_EQ(numberOfProducers, client.getNumberOfProducers());

    producer.close();
    numberOfProducers = 0;
    ASSERT_EQ(numberOfProducers, client.getNumberOfProducers());

    // PartitionedProducer
    int res = makePutRequest(
        "http://localhost:8080/admin/v2/persistent/public/default/" + partitionedTopic + "/partitions", "2");
    ASSERT_TRUE(res == 204 || res == 409) << "res: " << res;

    client.createProducer(partitionedTopic, producer);
    numberOfProducers = 2;
    ASSERT_EQ(numberOfProducers, client.getNumberOfProducers());
    producer.close();
    numberOfProducers = 0;
    ASSERT_EQ(numberOfProducers, client.getNumberOfProducers());

    // Consumer test
    uint64_t numberOfConsumers = 0;

    Consumer consumer1;
    client.subscribe(nonPartitionedTopic, "consumer-1", consumer1);
    numberOfConsumers = 1;
    ASSERT_EQ(numberOfConsumers, client.getNumberOfConsumers());

    consumer1.close();
    numberOfConsumers = 0;
    ASSERT_EQ(numberOfConsumers, client.getNumberOfConsumers());

    Consumer consumer2;
    Consumer consumer3;
    client.subscribe(partitionedTopic, "consumer-2", consumer2);
    numberOfConsumers = 2;
    ASSERT_EQ(numberOfConsumers, client.getNumberOfConsumers());
    client.subscribe(nonPartitionedTopic, "consumer-3", consumer3);
    numberOfConsumers = 3;
    ASSERT_EQ(numberOfConsumers, client.getNumberOfConsumers());
    consumer2.close();
    consumer3.close();
    numberOfConsumers = 0;
    ASSERT_EQ(numberOfConsumers, client.getNumberOfConsumers());

    client.close();
}

TEST(ClientTest, testReferenceCount) {
    Client client(lookupUrl);
    const std::string topic = "client-test-reference-count-" + std::to_string(time(nullptr));

    auto &producers = PulsarFriend::getProducers(client);
    auto &consumers = PulsarFriend::getConsumers(client);
    ReaderImplWeakPtr readerWeakPtr;

    {
        Producer producer;
        ASSERT_EQ(ResultOk, client.createProducer(topic, producer));
        ASSERT_EQ(producers.size(), 1);

        producers.forEachValue([](const ProducerImplBaseWeakPtr &weakProducer) {
            LOG_INFO("Reference count of producer: " << weakProducer.use_count());
            ASSERT_FALSE(weakProducer.expired());
        });

        Consumer consumer;
        ASSERT_EQ(ResultOk, client.subscribe(topic, "my-sub", consumer));
        ASSERT_EQ(consumers.size(), 1);

        ReaderConfiguration readerConf;
        Reader reader;
        ASSERT_EQ(ResultOk,
                  client.createReader(topic + "-reader", MessageId::earliest(), readerConf, reader));
        ASSERT_EQ(consumers.size(), 2);

        consumers.forEachValue([](const ConsumerImplBaseWeakPtr &weakConsumer) {
            LOG_INFO("Reference count of consumer: " << weakConsumer.use_count());
            ASSERT_FALSE(weakConsumer.expired());
        });

        readerWeakPtr = PulsarFriend::getReaderImplWeakPtr(reader);
        ASSERT_TRUE(readerWeakPtr.use_count() > 0);
        LOG_INFO("Reference count of the reader: " << readerWeakPtr.use_count());
    }

    waitUntil(std::chrono::seconds(3), [&] {
        return producers.size() == 0 && consumers.size() == 0 && readerWeakPtr.use_count() == 0;
    });
    EXPECT_EQ(producers.size(), 0);
    EXPECT_EQ(consumers.size(), 0);
    EXPECT_EQ(readerWeakPtr.use_count(), 0);
    client.close();
}

TEST(ClientTest, testWrongListener) {
    const std::string topic = "client-test-wrong-listener-" + std::to_string(time(nullptr));
    auto httpCode = makePutRequest(
        "http://localhost:8080/admin/v2/persistent/public/default/" + topic + "/partitions", "3");
    LOG_INFO("create " << topic << ": " << httpCode);

    Client client(lookupUrl, ClientConfiguration().setListenerName("test"));
    Producer producer;
    ASSERT_EQ(ResultServiceUnitNotReady, client.createProducer(topic, producer));
    ASSERT_EQ(ResultProducerNotInitialized, producer.close());
    ASSERT_EQ(PulsarFriend::getProducers(client).size(), 0);
    ASSERT_EQ(ResultOk, client.close());

    // The connection will be closed when the consumer failed, we must recreate the Client. Otherwise, the
    // creation of Consumer or Reader could fail with ResultConnectError.
    client = Client(lookupUrl, ClientConfiguration().setListenerName("test"));
    Consumer consumer;
    ASSERT_EQ(ResultServiceUnitNotReady, client.subscribe(topic, "sub", consumer));
    ASSERT_EQ(ResultConsumerNotInitialized, consumer.close());

    ASSERT_EQ(PulsarFriend::getConsumers(client).size(), 0);
    ASSERT_EQ(ResultOk, client.close());

    client = Client(lookupUrl, ClientConfiguration().setListenerName("test"));

    Consumer multiTopicsConsumer;
    ASSERT_EQ(ResultServiceUnitNotReady,
              client.subscribe({topic + "-partition-0", topic + "-partition-1", topic + "-partition-2"},
                               "sub", multiTopicsConsumer));

    ASSERT_EQ(PulsarFriend::getConsumers(client).size(), 0);
    ASSERT_EQ(ResultOk, client.close());

    // Currently Reader can only read a non-partitioned topic in C++ client
    client = Client(lookupUrl, ClientConfiguration().setListenerName("test"));

    // Currently Reader can only read a non-partitioned topic in C++ client
    Reader reader;
    ASSERT_EQ(ResultServiceUnitNotReady,
              client.createReader(topic + "-partition-0", MessageId::earliest(), {}, reader));
    ASSERT_EQ(ResultConsumerNotInitialized, reader.close());
    ASSERT_EQ(PulsarFriend::getConsumers(client).size(), 0);
    ASSERT_EQ(ResultOk, client.close());
}

TEST(ClientTest, testMultiBrokerUrl) {
    const std::string topic = "client-test-multi-broker-url-" + std::to_string(time(nullptr));
    Client client("pulsar://localhost:6000,localhost");  // the 1st address is not reachable

    Producer producer;
    PulsarFriend::setServiceUrlIndex(client, 0);
    ASSERT_EQ(ResultOk, client.createProducer(topic, producer));

    Consumer consumer;
    PulsarFriend::setServiceUrlIndex(client, 0);
    ASSERT_EQ(ResultOk, client.subscribe(topic, "sub", consumer));

    Reader reader;
    PulsarFriend::setServiceUrlIndex(client, 0);
    ASSERT_EQ(ResultOk, client.createReader(topic, MessageId::earliest(), {}, reader));
    client.close();
}

TEST(ClientTest, testCloseClient) {
    const std::string topic = "client-test-close-client-" + std::to_string(time(nullptr));

    for (int i = 0; i < 1000; ++i) {
        Client client(lookupUrl);
        client.createProducerAsync(topic, [](Result result, Producer producer) { producer.close(); });
        // simulate different time interval before close
        auto t0 = std::chrono::steady_clock::now();
        while ((std::chrono::steady_clock::now() - t0) < std::chrono::microseconds(i)) {
        }
        client.close();
    }
}

namespace pulsar {

class PulsarWrapper {
   public:
    static ClientConfiguration createConfig(const std::string &description) {
        ClientConfiguration conf;
        conf.setDescription(description);
        return conf;
    }
};

}  // namespace pulsar

// When `subscription` is empty, get client versions of the producers.
// Otherwise, get client versions of the consumers under the subscribe.
static std::vector<std::string> getClientVersions(const std::string &topic, std::string subscription = "") {
    const auto url = adminUrl + "admin/v2/persistent/public/default/" + topic + "/stats";
    std::string responseData;
    int res = makeGetRequest(url, responseData);
    if (res != 200) {
        LOG_ERROR(url << " failed: " << res);
        return {};
    }

    std::stringstream stream;
    stream << responseData;
    boost::property_tree::ptree root;
    boost::property_tree::read_json(stream, root);
    std::vector<std::string> versions;
    if (subscription.empty()) {
        for (auto &child : root.get_child("publishers")) {
            versions.emplace_back(child.second.get<std::string>("clientVersion"));
        }
    } else {
        auto consumers = root.get_child("subscriptions").get_child_optional(subscription);
        if (consumers) {
            for (auto &child : consumers.value().get_child("consumers")) {
                versions.emplace_back(child.second.get<std::string>("clientVersion"));
            }
        }
    }
    std::sort(versions.begin(), versions.end());
    return versions;
}

TEST(ClientTest, testClientVersion) {
    const std::string topic = "testClientVersion" + std::to_string(time(nullptr));
    const std::string expectedVersion = std::string("Pulsar-CPP-v") + PULSAR_VERSION_STR;

    Client client(lookupUrl);
    Client client2(lookupUrl, PulsarWrapper::createConfig("forked"));

    std::string responseData;

    ASSERT_TRUE(getClientVersions(topic).empty());
    Producer producer;
    ASSERT_EQ(ResultOk, client.createProducer(topic, producer));
    ASSERT_EQ(getClientVersions(topic), (std::vector<std::string>{expectedVersion}));

    Producer producer2;
    ASSERT_EQ(ResultOk, client2.createProducer(topic, producer2));
    ASSERT_EQ(getClientVersions(topic),
              (std::vector<std::string>{expectedVersion, expectedVersion + "-forked"}));

    producer.close();

    ASSERT_TRUE(getClientVersions(topic, "consumer-1").empty());
    auto consumerConf = ConsumerConfiguration{}.setConsumerType(ConsumerType::ConsumerFailover);
    Consumer consumer;
    ASSERT_EQ(ResultOk, client.subscribe(topic, "consumer-1", consumerConf, consumer));
    ASSERT_EQ(getClientVersions(topic, "consumer-1"), (std::vector<std::string>{expectedVersion}));

    Consumer consumer2;
    ASSERT_EQ(ResultOk, client2.subscribe(topic, "consumer-1", consumerConf, consumer2));
    ASSERT_EQ(getClientVersions(topic, "consumer-1"),
              (std::vector<std::string>{expectedVersion, expectedVersion + "-forked"}));

    consumer.close();

    client.close();
}
