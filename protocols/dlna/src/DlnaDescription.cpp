#include "DlnaDescription.h"

#include <sstream>

namespace adisplay::dlna::description {
namespace {

// SCPD 的骨架。每个服务都会带上自己那几个动作与状态变量 ——
// 声明得比实际实现多没关系，声明少了才会出问题（手机会认为不支持该动作）。
struct ServiceSpec {
    const char* service_type;
    const char* service_id;
    const char* actions;
    const char* state_variables;
};

const ServiceSpec kAvTransportSpec = {
    kAvTransportType,
    kAvTransportId,
    R"(
      <action><name>SetAVTransportURI</name>
        <argumentList>
          <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
          <argument><name>CurrentURI</name><direction>in</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>
          <argument><name>CurrentURIMetaData</name><direction>in</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>
        </argumentList>
      </action>
      <action><name>GetMediaInfo</name>
        <argumentList>
          <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
          <argument><name>NrTracks</name><direction>out</direction><relatedStateVariable>NumberOfTracks</relatedStateVariable></argument>
          <argument><name>MediaDuration</name><direction>out</direction><relatedStateVariable>CurrentMediaDuration</relatedStateVariable></argument>
          <argument><name>CurrentURI</name><direction>out</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>
          <argument><name>CurrentURIMetaData</name><direction>out</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>
          <argument><name>NextURI</name><direction>out</direction><relatedStateVariable>NextAVTransportURI</relatedStateVariable></argument>
          <argument><name>NextURIMetaData</name><direction>out</direction><relatedStateVariable>NextAVTransportURIMetaData</relatedStateVariable></argument>
          <argument><name>PlayMedium</name><direction>out</direction><relatedStateVariable>PlaybackStorageMedium</relatedStateVariable></argument>
          <argument><name>RecordMedium</name><direction>out</direction><relatedStateVariable>RecordStorageMedium</relatedStateVariable></argument>
          <argument><name>WriteStatus</name><direction>out</direction><relatedStateVariable>RecordMediumWriteStatus</relatedStateVariable></argument>
        </argumentList>
      </action>
      <action><name>GetTransportInfo</name>
        <argumentList>
          <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
          <argument><name>CurrentTransportState</name><direction>out</direction><relatedStateVariable>TransportState</relatedStateVariable></argument>
          <argument><name>CurrentTransportStatus</name><direction>out</direction><relatedStateVariable>TransportStatus</relatedStateVariable></argument>
          <argument><name>CurrentSpeed</name><direction>out</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>
        </argumentList>
      </action>
      <action><name>GetPositionInfo</name>
        <argumentList>
          <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
          <argument><name>Track</name><direction>out</direction><relatedStateVariable>CurrentTrack</relatedStateVariable></argument>
          <argument><name>TrackDuration</name><direction>out</direction><relatedStateVariable>CurrentTrackDuration</relatedStateVariable></argument>
          <argument><name>TrackMetaData</name><direction>out</direction><relatedStateVariable>CurrentTrackMetaData</relatedStateVariable></argument>
          <argument><name>TrackURI</name><direction>out</direction><relatedStateVariable>CurrentTrackURI</relatedStateVariable></argument>
          <argument><name>RelTime</name><direction>out</direction><relatedStateVariable>RelativeTimePosition</relatedStateVariable></argument>
          <argument><name>AbsTime</name><direction>out</direction><relatedStateVariable>AbsoluteTimePosition</relatedStateVariable></argument>
          <argument><name>RelCount</name><direction>out</direction><relatedStateVariable>RelativeCounterPosition</relatedStateVariable></argument>
          <argument><name>AbsCount</name><direction>out</direction><relatedStateVariable>AbsoluteCounterPosition</relatedStateVariable></argument>
        </argumentList>
      </action>
      <action><name>Play</name>
        <argumentList>
          <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
          <argument><name>Speed</name><direction>in</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>
        </argumentList>
      </action>
      <action><name>Pause</name>
        <argumentList>
          <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
        </argumentList>
      </action>
      <action><name>Stop</name>
        <argumentList>
          <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
        </argumentList>
      </action>
      <action><name>Seek</name>
        <argumentList>
          <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
          <argument><name>Unit</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SeekMode</relatedStateVariable></argument>
          <argument><name>Target</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SeekTarget</relatedStateVariable></argument>
        </argumentList>
      </action>)",
    R"(
      <stateVariable sendEvents="yes"><name>TransportState</name><dataType>string</dataType>
        <allowedValueList>
          <allowedValue>STOPPED</allowedValue>
          <allowedValue>PLAYING</allowedValue>
          <allowedValue>PAUSED_PLAYBACK</allowedValue>
          <allowedValue>TRANSITIONING</allowedValue>
          <allowedValue>NO_MEDIA_PRESENT</allowedValue>
        </allowedValueList>
      </stateVariable>
      <stateVariable sendEvents="no"><name>TransportStatus</name><dataType>string</dataType>
        <allowedValueList>
          <allowedValue>OK</allowedValue>
          <allowedValue>ERROR_OCCURRED</allowedValue>
        </allowedValueList>
      </stateVariable>
      <stateVariable sendEvents="no"><name>TransportPlaySpeed</name><dataType>string</dataType>
        <allowedValueList><allowedValue>1</allowedValue></allowedValueList>
      </stateVariable>
      <stateVariable sendEvents="no"><name>AVTransportURI</name><dataType>string</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>AVTransportURIMetaData</name><dataType>string</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>NextAVTransportURI</name><dataType>string</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>NextAVTransportURIMetaData</name><dataType>string</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>CurrentMediaDuration</name><dataType>string</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>CurrentTrack</name><dataType>ui4</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>CurrentTrackDuration</name><dataType>string</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>CurrentTrackMetaData</name><dataType>string</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>CurrentTrackURI</name><dataType>string</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>RelativeTimePosition</name><dataType>string</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>AbsoluteTimePosition</name><dataType>string</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>RelativeCounterPosition</name><dataType>i4</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>AbsoluteCounterPosition</name><dataType>i4</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>NumberOfTracks</name><dataType>ui4</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>PlaybackStorageMedium</name><dataType>string</dataType>
        <allowedValueList>
          <allowedValue>NONE</allowedValue>
          <allowedValue>NETWORK</allowedValue>
        </allowedValueList>
      </stateVariable>
      <stateVariable sendEvents="no"><name>RecordStorageMedium</name><dataType>string</dataType>
        <allowedValueList><allowedValue>NOT_IMPLEMENTED</allowedValue></allowedValueList>
      </stateVariable>
      <stateVariable sendEvents="no"><name>RecordMediumWriteStatus</name><dataType>string</dataType>
        <allowedValueList><allowedValue>NOT_IMPLEMENTED</allowedValue></allowedValueList>
      </stateVariable>
      <stateVariable sendEvents="no"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>A_ARG_TYPE_SeekMode</name><dataType>string</dataType>
        <allowedValueList>
          <allowedValue>REL_TIME</allowedValue>
          <allowedValue>ABS_TIME</allowedValue>
          <allowedValue>TRACK_NR</allowedValue>
        </allowedValueList>
      </stateVariable>
      <stateVariable sendEvents="no"><name>A_ARG_TYPE_SeekTarget</name><dataType>string</dataType></stateVariable>)",
};

const ServiceSpec kRenderingControlSpec = {
    kRenderingControlType,
    kRenderingControlId,
    R"(
      <action><name>GetVolume</name>
        <argumentList>
          <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
          <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>
          <argument><name>CurrentVolume</name><direction>out</direction><relatedStateVariable>Volume</relatedStateVariable></argument>
        </argumentList>
      </action>
      <action><name>SetVolume</name>
        <argumentList>
          <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
          <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>
          <argument><name>DesiredVolume</name><direction>in</direction><relatedStateVariable>Volume</relatedStateVariable></argument>
        </argumentList>
      </action>
      <action><name>GetMute</name>
        <argumentList>
          <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
          <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>
          <argument><name>CurrentMute</name><direction>out</direction><relatedStateVariable>Mute</relatedStateVariable></argument>
        </argumentList>
      </action>
      <action><name>SetMute</name>
        <argumentList>
          <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
          <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>
          <argument><name>DesiredMute</name><direction>in</direction><relatedStateVariable>Mute</relatedStateVariable></argument>
        </argumentList>
      </action>)",
    R"(
      <stateVariable sendEvents="yes"><name>Volume</name><dataType>ui2</dataType>
        <allowedValueRange><minimum>0</minimum><maximum>100</maximum><step>1</step></allowedValueRange>
      </stateVariable>
      <stateVariable sendEvents="yes"><name>Mute</name><dataType>boolean</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>A_ARG_TYPE_Channel</name><dataType>string</dataType>
        <allowedValueList><allowedValue>Master</allowedValue></allowedValueList>
      </stateVariable>)",
};

const ServiceSpec kConnectionManagerSpec = {
    kConnectionManagerType,
    kConnectionManagerId,
    R"(
      <action><name>GetProtocolInfo</name>
        <argumentList>
          <argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument>
          <argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument>
        </argumentList>
      </action>
      <action><name>GetCurrentConnectionIDs</name>
        <argumentList>
          <argument><name>ConnectionIDs</name><direction>out</direction><relatedStateVariable>CurrentConnectionIDs</relatedStateVariable></argument>
        </argumentList>
      </action>
      <action><name>GetCurrentConnectionInfo</name>
        <argumentList>
          <argument><name>ConnectionID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>
          <argument><name>RcsID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_RcsID</relatedStateVariable></argument>
          <argument><name>AVTransportID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_AVTransportID</relatedStateVariable></argument>
          <argument><name>ProtocolInfo</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ProtocolInfo</relatedStateVariable></argument>
          <argument><name>PeerConnectionManager</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionManager</relatedStateVariable></argument>
          <argument><name>PeerConnectionID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>
          <argument><name>Direction</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Direction</relatedStateVariable></argument>
          <argument><name>Status</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionStatus</relatedStateVariable></argument>
        </argumentList>
      </action>)",
    R"(
      <stateVariable sendEvents="yes"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>
      <stateVariable sendEvents="yes"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>
      <stateVariable sendEvents="yes"><name>CurrentConnectionIDs</name><dataType>string</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionStatus</name><dataType>string</dataType>
        <allowedValueList>
          <allowedValue>OK</allowedValue>
          <allowedValue>ContentFormatMismatch</allowedValue>
          <allowedValue>InsufficientBandwidth</allowedValue>
          <allowedValue>UnreliableChannel</allowedValue>
          <allowedValue>Unknown</allowedValue>
        </allowedValueList>
      </stateVariable>
      <stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionManager</name><dataType>string</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>A_ARG_TYPE_Direction</name><dataType>string</dataType>
        <allowedValueList>
          <allowedValue>Input</allowedValue>
          <allowedValue>Output</allowedValue>
        </allowedValueList>
      </stateVariable>
      <stateVariable sendEvents="no"><name>A_ARG_TYPE_ProtocolInfo</name><dataType>string</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionID</name><dataType>i4</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>A_ARG_TYPE_AVTransportID</name><dataType>i4</dataType></stateVariable>
      <stateVariable sendEvents="no"><name>A_ARG_TYPE_RcsID</name><dataType>i4</dataType></stateVariable>)",
};

const ServiceSpec* find_spec(const std::string& service_type) {
    if (service_type == kAvTransportType) {
        return &kAvTransportSpec;
    }
    if (service_type == kRenderingControlType) {
        return &kRenderingControlSpec;
    }
    if (service_type == kConnectionManagerType) {
        return &kConnectionManagerSpec;
    }
    return nullptr;
}

void append_service(std::ostringstream& out, const ServiceSpec& spec, const std::string& base_url) {
    out << "    <service>\n"
        << "      <serviceType>" << spec.service_type << "</serviceType>\n"
        << "      <serviceId>" << spec.service_id << "</serviceId>\n"
        << "      <SCPDURL>/scpd/" << spec.service_id << ".xml</SCPDURL>\n"
        << "      <controlURL>/control</controlURL>\n"
        << "      <eventSubURL>/event</eventSubURL>\n"
        << "    </service>\n";
    (void)base_url;   // 各 URL 用相对路径，手机端会基于 LOCATION 解析
}

}  // namespace

std::string escape_xml(const std::string& text) {
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (const char c : text) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

std::string build_device_description(const DlnaConfig& config, const std::string& base_url) {
    const std::string escaped_name = escape_xml(config.device_name);
    const std::string escaped_manufacturer = escape_xml(config.manufacturer);
    const std::string escaped_model = escape_xml(config.model_name);
    const std::string escaped_model_number = escape_xml(config.model_number);
    const std::string escaped_uuid = escape_xml(config.uuid);

    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        // dlna 命名空间必须声明在 root 上：下面要用 dlna:X_DLNADOC。
        << "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" "
        << "xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">\n"
        << "  <specVersion><major>1</major><minor>0</minor></specVersion>\n"
        << "  <device>\n"
        << "    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>\n"
        << "    <friendlyName>" << escaped_name << "</friendlyName>\n"
        << "    <manufacturer>" << escaped_manufacturer << "</manufacturer>\n"
        // manufacturerURL 与 modelURL 留空但必须存在 —— 少数手机端会检查
        // 元素是否存在（而不是内容是否为空）。
        << "    <manufacturerURL></manufacturerURL>\n"
        << "    <modelDescription>ADisplay 投屏接收端</modelDescription>\n"
        << "    <modelName>" << escaped_model << "</modelName>\n"
        << "    <modelNumber>" << escaped_model_number << "</modelNumber>\n"
        << "    <modelURL></modelURL>\n"
        << "    <serialNumber></serialNumber>\n"
        // UDN 里的 "uuid:" 前缀不能少，少了手机端认不出这是个 UPnP 设备。
        << "    <UDN>uuid:" << escaped_uuid << "</UDN>\n"
        // DLNA 认证标记，必须在 UDN 之后。
        //
        // UPnP 规范对 <device> 子元素的顺序有严格要求：
        //   deviceType → friendlyName → manufacturer → manufacturerURL →
        //   modelDescription → modelName → modelNumber → modelURL →
        //   serialNumber → UDN → [X_DLNADOC] → iconList → serviceList → ...
        //
        // 顺序不对的话，严格的解析器（实测 libupnp 就是）会直接放弃整份
        // 描述 —— 它会来 GET 一次 description.xml，然后什么都不做，
        // 在客户端看来就是「搜不到设备」。
        << "    <dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>\n"
        // iconList 在 UDN / X_DLNADOC 之后、serviceList 之前，这个顺序同样是
        // 规范要求的。不少客户端会检查图标是否存在，缺失时不显示设备。
        << "    <iconList>\n"
        << "      <icon>\n"
        << "        <mimetype>image/png</mimetype>\n"
        << "        <width>48</width>\n"
        << "        <height>48</height>\n"
        << "        <depth>24</depth>\n"
        << "        <url>/icon.png</url>\n"
        << "      </icon>\n"
        << "    </iconList>\n"
        << "    <serviceList>\n";

    append_service(out, kAvTransportSpec, base_url);
    append_service(out, kRenderingControlSpec, base_url);
    append_service(out, kConnectionManagerSpec, base_url);

    out << "    </serviceList>\n"
        // presentationURL 是可选元素，但位置固定在 serviceList 之后。
        // 部分客户端会用它做「设备详情」的跳转入口。
        << "    <presentationURL>/</presentationURL>\n"
        << "  </device>\n"
        << "</root>\n";
    return out.str();
}

std::string build_service_description(const std::string& service_type) {
    const ServiceSpec* spec = find_spec(service_type);
    if (spec == nullptr) {
        return std::string();
    }

    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        << "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\n"
        << "  <specVersion><major>1</major><minor>0</minor></specVersion>\n"
        << "  <actionList>" << spec->actions << "\n  </actionList>\n"
        << "  <serviceStateTable>" << spec->state_variables << "\n  </serviceStateTable>\n"
        << "</scpd>\n";
    return out.str();
}

std::string extract_title_from_metadata(const std::string& metadata) {
    // DIDL-Lite 里的标题形如 <dc:title>片名</dc:title>。
    // 不引入 XML 解析只为取一个字段 —— 但要做基本的实体反转义，
    // 否则带 & 的片名会显示成 &amp;。
    const std::string open_tag = "<dc:title>";
    const std::string close_tag = "</dc:title>";

    const std::size_t begin = metadata.find(open_tag);
    if (begin == std::string::npos) {
        return std::string();
    }
    const std::size_t content_begin = begin + open_tag.size();
    const std::size_t end = metadata.find(close_tag, content_begin);
    if (end == std::string::npos || end <= content_begin) {
        return std::string();
    }

    std::string title = metadata.substr(content_begin, end - content_begin);

    const auto replace_all = [](std::string& text, const std::string& from, const std::string& to) {
        std::size_t position = 0;
        while ((position = text.find(from, position)) != std::string::npos) {
            text.replace(position, from.size(), to);
            position += to.size();
        }
    };
    // 顺序有讲究：&amp; 必须最后还原，否则 "&amp;lt;" 会被错误地解成 "<"。
    replace_all(title, "&lt;", "<");
    replace_all(title, "&gt;", ">");
    replace_all(title, "&quot;", "\"");
    replace_all(title, "&apos;", "'");
    replace_all(title, "&amp;", "&");

    return title;
}

}  // namespace adisplay::dlna::description
