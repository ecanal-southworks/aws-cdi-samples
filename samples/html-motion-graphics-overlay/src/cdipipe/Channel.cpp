#include <iostream>
#include <iomanip>
#include <cassert>

#include <boost/asio.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/thread.hpp>

#include "PayloadBuffer.h"
#include "Channel.h"
#include "Errors.h"
#include "Exceptions.h"
#include "VideoStream.h"
#include "AudioStream.h"
#include "AncillaryStream.h"

using namespace boost::asio;
using std::cout;
using std::setw;
using std::left;

CdiTools::Channel::Channel(const std::string& name)
    : name_{ name }
    , logger_{ name }
{
}

CdiTools::Channel::~Channel()
{
    shutdown();
}

void CdiTools::Channel::start(ChannelHandler handler, int thread_pool_size)
{
    active_ = std::make_unique<boost::asio::io_context::work>(io_);

    LOG_INFO << "Waiting for channel connections to be ready...";
    open_connections(handler);

    if (thread_pool_size > 0) {
        boost::thread_group pool;
        for (int i = 0; i < thread_pool_size; i++) {
            pool.create_thread([&]() { io_.run(); });
        }

        pool.join_all();
    }
    else {
        io_.run();
    }

    LOG_INFO << "Channel shut down sucessfully.";
}

void CdiTools::Channel::open_connections(ChannelHandler handler)
{
    for (auto&& connection : connections_) {
        auto connection_handler = [=](const std::error_code& ec) {
            if (!ec) {
                LOG_INFO << "Connection '" << connection->get_name() << "' established successfully.";
                // start the read loop for the input connections
                if (is_active() && ConnectionDirection::In == connection->get_direction()) {
                    if (connection->get_type() != ConnectionType::Cdi) {
                        async_read(connection, std::error_code(), handler);
                    }
                    else {
                        // set the receive handler for CDI, which starts to receive as soon as the connection is opened
                        connection->async_receive(
                            std::bind(&Channel::read_complete, shared_from_this(), connection, std::placeholders::_1, std::placeholders::_2, handler));
                    }

                    // clear output buffer for any stream associated with this connection
                    for (auto&& stream : get_connection_streams(connection->get_name())) {
                        for (auto&& output : get_stream_connections(stream->id(), ConnectionDirection::Out)) {
                            auto& buffer = get_connection_buffer(output->get_name());
                            buffer.clear();
                        }
                    }
                }
                else {
                    async_write(connection, std::error_code(), handler);
                }
            }
            else {
                LOG_ERROR << "Connection '" << connection->get_name() << "' failed: " << ec.message() << ".";
                handler(ec);
            }
        };

        if (connection->get_status() == ConnectionStatus::Closed) {
            LOG_DEBUG << "Opening connection '" << connection->get_name() << "'...";
            if (connection->get_mode() == ConnectionMode::Client) {
                connection->async_connect(connection_handler);
            }
            else {
                connection->async_accept(connection_handler);
            }
        }
    }
}

void CdiTools::Channel::async_read(
    std::shared_ptr<IConnection> connection,
    const std::error_code& ec,
    ChannelHandler handler)
{
    if (!is_active()) return;

    if (ec) {
        if (connection->get_status() != ConnectionStatus::Open) {
            LOG_WARNING << "Output connection '" << connection->get_name() << "' is not ready.";
            open_connections(handler);
            return;
        }

        LOG_WARNING << "Error receiving a payload: " << ec.message();
    }

    // receiving next payload for this connection
    connection->async_receive(
        std::bind(&Channel::read_complete, shared_from_this(), connection, std::placeholders::_1, std::placeholders::_2, handler));
}

void CdiTools::Channel::read_complete(
    std::shared_ptr<IConnection> connection,
    const std::error_code& ec,
    Payload payload,
    ChannelHandler handler)
{
    // determine the payload stream and retrieve its output connections
    auto stream = get_stream(payload->stream_identifier());
    auto payloads_received = stream->received_payload();
    if (ec) {
        stream->payload_error();
    }

    if (!ec) {
        // queue the payload for transmission by each output connection in stream
        auto connections = get_stream_connections(payload->stream_identifier(), ConnectionDirection::Out);
        for (auto&& output_connection : connections) {
            if (ConnectionStatus::Open != output_connection->get_status()) {
                open_connections(handler);
                continue;
            }

            auto& connection_name = output_connection->get_name();
            auto& connection_buffer = get_connection_buffer(connection_name);
            if (connection_buffer.is_full()) {
                stream->payload_error();
            }

            connection_buffer.enqueue(payload);
            LOG_DEBUG << "Received payload #" << payload->stream_identifier() << ":" << payloads_received
#ifdef TRACE_PAYLOADS
                << " (" << payload->sequence() << ")"
#endif
                << ", size: " << payload->get_size()
                << ", queue length/size: " << connection_buffer.size() << "/" << connection_buffer.capacity()
                << ".";
        }
    }

    // resume read loop
    if (connection->get_type() != ConnectionType::Cdi) {
        async_read(connection, ec, handler);
    }
}

void CdiTools::Channel::async_write(
    std::shared_ptr<IConnection> connection,
    const std::error_code& ec,
    ChannelHandler handler)
{
    if (!is_active()) return;

    if (ec) {
        if (connection->get_status() != ConnectionStatus::Open) {
            LOG_WARNING << "Output connection '" << connection->get_name() << "' is not ready.";
            open_connections(handler);
            return;
        }

        LOG_WARNING << "Error transmitting a payload: " << ec.message();
    }

    auto& connection_buffer = get_connection_buffer(connection->get_name());
    if (connection_buffer.is_empty()) {
        std::this_thread::yield();
        post(io_, std::bind(&Channel::async_write, shared_from_this(), connection, ec, handler));
        return;
    }

    auto payload = connection_buffer.front();
    auto stream = get_stream(payload->stream_identifier());
    // TODO: payloads transmitted might be wrong if there are multiple outputs
    LOG_TRACE << "Transmitting payload #" << payload->stream_identifier() << ":" << stream->get_payloads_transmitted() + 1
#ifdef TRACE_PAYLOADS
        << " (" << payload->sequence() << ")"
#endif
        << ", size: " << payload->get_size()
        << ", queue length/size: " << connection_buffer.size() << "/" << connection_buffer.capacity()
        << "...";

    connection->async_transmit(
        payload,
        std::bind(&Channel::write_complete, shared_from_this(), connection, stream, std::placeholders::_1, handler));
}

void CdiTools::Channel::write_complete(
    std::shared_ptr<IConnection> connection,
    std::shared_ptr<Stream> stream,
    const std::error_code& ec,
    ChannelHandler handler)
{
    auto payloads_transmitted = stream->transmitted_payload();
    if (ec) {
        stream->payload_error();
    }

    auto& connection_buffer = get_connection_buffer(connection->get_name());
    connection_buffer.pop_front();

    if (!ec) {
#ifdef TRACE_PAYLOADS
        auto payload = connection_buffer.front();
        auto sequence = payload != nullptr ? payload->sequence() : 0;
        auto size = payload != nullptr ? payload->get_size() : 0;
#endif

        LOG_DEBUG << "Transmitted payload #" << stream->id() << ":" << payloads_transmitted
#ifdef TRACE_PAYLOADS
            << " (" << sequence << ")"
            << ", size: " << size
#endif
            << ", queue length/size: " << connection_buffer.size() << "/" << connection_buffer.capacity()
            << ".";
    }

    async_write(connection, ec, handler);
}

CdiTools::PayloadBuffer& CdiTools::Channel::get_connection_buffer(const std::string& connection_name)
{
    const auto& it = connection_buffers_.find(connection_name);
    assert(it != connection_buffers_.end());

    auto& connection_state = it->second;
    auto& buffer = connection_state.first;
    size_t buffer_size = buffer.size();
    if (buffer.is_full()) {
        if (!connection_state.second) {
            LOG_WARNING << "Receive buffer for connection '" << connection_name << "' is full"
                << ", capacity: " << buffer.capacity()
                << ". One or more payloads will be discarded.";
            connection_state.second = true;
        }
    }

    if (connection_state.second) {
        size_t buffer_capacity = buffer.capacity();
        const size_t low_water_mark = static_cast<size_t>(buffer_capacity * 0.8);
        connection_state.second = buffer_size > low_water_mark;
    }

    return it->second.first;
}

void CdiTools::Channel::shutdown()
{
    if (active_ == nullptr) return;

    LOG_DEBUG << "Channel is shutting down...";

    active_.reset();

    for (auto&& connection : connections_) {
        std::error_code ec;
        connection->disconnect(ec);
        if (ec) {
            LOG_ERROR << "Connection'" << connection->get_name() << "' could not be closed: " << ec.message() << ", code: " << ec.value() << ".";
        }
        else {
            LOG_INFO << "Connection '" << connection->get_name() << "' closed successfully.";
        }
    }

    io_.stop();
}

std::shared_ptr<CdiTools::IConnection> CdiTools::Channel::add_input(ConnectionType connection_type, const std::string& name,
    const std::string& host_name, unsigned short port_number, ConnectionMode connection_mode, size_t buffer_size)
{
    auto connection = Connection::get_connection(connection_type, name, host_name, port_number, connection_mode, ConnectionDirection::In, io_);

    connection_buffers_.try_emplace(name, buffer_size, false);

    connections_.push_back(connection);

    return connection;
}

std::shared_ptr<CdiTools::IConnection> CdiTools::Channel::add_output(ConnectionType connection_type, const std::string& name,
    const std::string& host_name, unsigned short port_number, ConnectionMode connection_mode, size_t buffer_size)
{
    auto connection = Connection::get_connection(connection_type, name, host_name, port_number, connection_mode, ConnectionDirection::Out, io_);

    connection_buffers_.try_emplace(name, buffer_size, false);

    connections_.push_back(connection);

    return connection;
}

std::shared_ptr<CdiTools::Stream> CdiTools::Channel::add_video_stream(
    uint16_t stream_identifier, int frame_width, int frame_height,
    int bytes_per_pixel, int frame_rate_numerator, int frame_rate_denominator)
{
    // TODO: validate that stream_identifier is not already defined
    auto stream = std::make_shared<VideoStream>(stream_identifier, frame_width,
        frame_height, bytes_per_pixel, frame_rate_numerator, frame_rate_denominator);
    streams_.push_back(stream);

    return stream;
}

std::shared_ptr<CdiTools::Stream> CdiTools::Channel::add_audio_stream(
    uint16_t stream_identifier, AudioChannelGrouping channel_grouping,
    AudioSamplingRate audio_sampling_rate, int bytes_per_sample, const std::string& language)
{
    // TODO: validate that stream_identifier is not already defined
    auto stream = std::make_shared<AudioStream>(stream_identifier, channel_grouping, audio_sampling_rate, bytes_per_sample, language);
    streams_.push_back(stream);

    return stream;
}

std::shared_ptr<CdiTools::Stream> CdiTools::Channel::add_ancillary_stream(uint16_t stream_identifier)
{
    // TODO: validate that stream_identifier is not already defined
    auto stream = std::make_shared<AncillaryStream>(stream_identifier);
    streams_.push_back(stream);

    return stream;
}

void CdiTools::Channel::map_stream(uint16_t stream_identifier, const std::string& connection_name)
{
    auto connection = std::find_if(connections_.begin(), connections_.end(),
        [&](const auto& connection) { return connection->get_name() == connection_name; });
    if (connection == connections_.end()) {
        throw InvalidConfigurationException(std::string("Failed to map unknown connection '" + connection_name + "'."));
    }

    if ((*connection)->get_direction() == ConnectionDirection::In) {
        auto stream_connections = get_stream_connections(stream_identifier, ConnectionDirection::In);
        if (stream_connections.size() > 0) {
            throw InvalidConfigurationException(
                std::string("Stream [") + std::to_string(stream_identifier) + "] is already assigned to connection '" + stream_connections[0]->get_name()
                + "' and cannot also be assigned to connection '" + connection_name + "'. Only a single input connection is allowed per stream.");
        }
    }

    (*connection)->add_stream(get_stream(stream_identifier));

    channel_map_.insert({ connection_name, stream_identifier });
}

void CdiTools::Channel::validate_configuration()
{
    for (auto&& connection : connections_) {
        auto streams = channel_map_.left.equal_range(connection->get_name());
        if (streams.first == streams.second) {
            throw InvalidConfigurationException(
                std::string("Connection '") + connection->get_name() + "' has no stream assigned.");
        }
    }
}

void CdiTools::Channel::show_configuration()
{
    std::vector<std::shared_ptr<IConnection>> inputs;
    std::vector<std::shared_ptr<IConnection>> outputs;
    std::partition_copy(begin(connections_), end(connections_),
        std::back_inserter(inputs), std::back_inserter(outputs),
        [](const auto& item) { return item->get_direction() == ConnectionDirection::In; });

    std::cout << "# Inputs\n";
    for (auto&& connection : inputs) {
        std::cout << "  [" << setw(12) << left << connection->get_name() << "] "
            << "type: " << typeid(*connection).name()
            << "\n";
        for (auto&& stream : get_connection_streams(connection->get_name())) {
            std::cout << "    stream: " << stream->id() << "\n";
        }
    }

    std::cout << "\n# Outputs\n";
    for (auto&& connection : outputs) {
        std::cout << "  [" << setw(12) << left << connection->get_name() << "] "
            << "type: " << typeid(*connection).name()
            << "\n";
        for (auto&& stream : get_connection_streams(connection->get_name())) {
            std::cout << "    stream: " << stream->id() << "\n";
        }
    }
}

std::shared_ptr<CdiTools::Stream> CdiTools::Channel::get_stream(uint16_t stream_identifier)
{
    auto stream = std::find_if(streams_.begin(), streams_.end(),
        [=](const auto& stream) { return stream->id() == stream_identifier; });

    if (stream == streams_.end()) {
        throw InvalidConfigurationException(
            std::string("An unrecognized stream [") + std::to_string(stream_identifier) + "] was specified.");
    }

    return *stream;
}

std::vector<std::shared_ptr<CdiTools::IConnection>> CdiTools::Channel::get_stream_connections(uint16_t stream_identifier, ConnectionDirection direction)
{
    std::vector<std::shared_ptr<IConnection>> stream_connections;
    for (auto&& map_entry : boost::make_iterator_range(channel_map_.right.equal_range(stream_identifier))) {
        auto connection = std::find_if(connections_.begin(), connections_.end(),
            [&](const auto& connection) { return connection->get_name() == map_entry.second; });
        if (connection == connections_.end())
        {
            throw InvalidConfigurationException(
                std::string("Stream [") + std::to_string(stream_identifier) + "] is mapped to an unknown connection '" + map_entry.second + "'.");
        }

        if (ConnectionDirection::Both == direction || (*connection)->get_direction() == direction) {
            stream_connections.push_back(*connection);
        }
    }

    return stream_connections;
}

std::vector<std::shared_ptr<CdiTools::Stream>> CdiTools::Channel::get_connection_streams(const std::string& connection_name)
{
    std::vector<std::shared_ptr<Stream>> connection_streams;
    for (auto&& map_entry : boost::make_iterator_range(channel_map_.left.equal_range(connection_name))) {
        auto stream = std::find_if(streams_.begin(), streams_.end(),
            [&](const auto& stream) { return stream->id() == map_entry.second; });
        if (stream == streams_.end())
        {
            throw InvalidConfigurationException(
                std::string("Connection '") + connection_name + "' is mapped to an unknown Stream [" + std::to_string(map_entry.second) + "].");
        }

        connection_streams.push_back(*stream);
    }

    return connection_streams;
}

void CdiTools::Channel::show_stream_connections(uint16_t stream_identifier, ConnectionDirection direction)
{
    std::cout << "stream: " << stream_identifier << "\n";
    const std::vector<std::shared_ptr<IConnection>>& connections = get_stream_connections(stream_identifier, direction);
    for (auto&& connection : connections) {
        std::cout << "connection: " << connection->get_name() << " (" << (connection->get_direction() == ConnectionDirection::In ? "input" : "output") << ")\n";
    }

    std::cout << "\n";
}
