#include "redclaw/diag/pointer_geometry_diagnostics.h"

#include <gtest/gtest.h>

#include <string>

using redclaw::diag::PointerGeometryRecord;
using redclaw::diag::PointerInputSampleGate;
using redclaw::diag::PointerInputSampleRecord;
using redclaw::diag::format_pointer_geometry_line;
using redclaw::diag::format_pointer_input_sample_line;
using redclaw::diag::pointer_mapping_signature;

TEST(PointerGeometryDiagnostics, GeometryLineContainsRequiredFields) {
    PointerGeometryRecord record;
    record.event = "capture_getdesc";
    record.backend = "wgc";
    record.display_id = "14c36-1";
    record.output_index = 1;
    record.origin_x = -1536;
    record.origin_y = 0;
    record.width = 1920;
    record.height = 1080;
    record.rotation = 0;
    record.revision = 3;
    record.region_x = 0;
    record.region_y = 0;
    record.region_width = 1920;
    record.region_height = 1080;
    record.region_revision = 3;
    record.process_dpi_awareness = 0;
    record.thread_dpi_context = "unaware";
    record.getdesc_valid = true;
    record.getdesc_left = -1536;
    record.getdesc_top = 0;
    record.getdesc_right = 0;
    record.getdesc_bottom = 864;
    record.virtual_valid = true;
    record.virtual_x = -1536;
    record.virtual_y = 0;
    record.virtual_width = 2880;
    record.virtual_height = 864;
    record.pma_virtual_valid = true;
    record.pma_virtual_x = -1920;
    record.pma_virtual_y = 0;
    record.pma_virtual_width = 3600;
    record.pma_virtual_height = 1080;

    const std::string line = format_pointer_geometry_line(record);
    EXPECT_EQ(line.find("Runtime pointer geometry "), 0U);
    EXPECT_NE(line.find("event=capture_getdesc"), std::string::npos);
    EXPECT_NE(line.find("backend=wgc"), std::string::npos);
    EXPECT_NE(line.find("display_id=14c36-1"), std::string::npos);
    EXPECT_NE(line.find("output_index=1"), std::string::npos);
    EXPECT_NE(line.find("geometry_origin_x=-1536"), std::string::npos);
    EXPECT_NE(line.find("geometry_origin_y=0"), std::string::npos);
    EXPECT_NE(line.find("geometry_width=1920"), std::string::npos);
    EXPECT_NE(line.find("geometry_height=1080"), std::string::npos);
    EXPECT_NE(line.find("geometry_rotation=0"), std::string::npos);
    EXPECT_NE(line.find("geometry_revision=3"), std::string::npos);
    EXPECT_NE(line.find("region_x=0"), std::string::npos);
    EXPECT_NE(line.find("region_width=1920"), std::string::npos);
    EXPECT_NE(line.find("region_revision=3"), std::string::npos);
    EXPECT_NE(line.find("process_dpi_awareness=0"), std::string::npos);
    EXPECT_NE(line.find("thread_dpi_context=unaware"), std::string::npos);
    EXPECT_NE(line.find("getdesc_valid=true"), std::string::npos);
    EXPECT_NE(line.find("getdesc_left=-1536"), std::string::npos);
    EXPECT_NE(line.find("virtual_valid=true"), std::string::npos);
    EXPECT_NE(line.find("virtual_x=-1536"), std::string::npos);
    EXPECT_NE(line.find("pma_virtual_valid=true"), std::string::npos);
    EXPECT_NE(line.find("pma_virtual_x=-1920"), std::string::npos);
    EXPECT_EQ(line.find('\n'), std::string::npos);
}

TEST(PointerGeometryDiagnostics, InputSampleLineContainsMappingPath) {
    PointerInputSampleRecord record;
    record.reason = "first_after_change";
    record.stage = "sendinput";
    record.sample_id = 7;
    record.normalized_x = 0;
    record.normalized_y = 32767;
    record.global_x = -1536;
    record.global_y = 540;
    record.sendinput_present = true;
    record.sendinput_dx = 1234;
    record.sendinput_dy = 20000;
    record.virtual_valid = true;
    record.virtual_x = -1920;
    record.virtual_y = 0;
    record.virtual_width = 3600;
    record.virtual_height = 1080;
    record.absolute = true;
    record.virtual_desk = true;
    record.origin_x = -1536;
    record.origin_y = 0;
    record.width = 1920;
    record.height = 1080;
    record.revision = 3;
    record.region_width = 1920;
    record.region_height = 1080;
    record.region_revision = 3;
    record.display_id = "14c36-1";

    const std::string line = format_pointer_input_sample_line(record);
    EXPECT_EQ(line.find("Runtime pointer input sample "), 0U);
    EXPECT_NE(line.find("reason=first_after_change"), std::string::npos);
    EXPECT_NE(line.find("stage=sendinput"), std::string::npos);
    EXPECT_NE(line.find("sample_id=7"), std::string::npos);
    EXPECT_NE(line.find("normalized_x=0"), std::string::npos);
    EXPECT_NE(line.find("normalized_y=32767"), std::string::npos);
    EXPECT_NE(line.find("global_x=-1536"), std::string::npos);
    EXPECT_NE(line.find("sendinput_present=true"), std::string::npos);
    EXPECT_NE(line.find("sendinput_dx=1234"), std::string::npos);
    EXPECT_NE(line.find("virtual_x=-1920"), std::string::npos);
    EXPECT_NE(line.find("virtual_width=3600"), std::string::npos);
    EXPECT_NE(line.find("absolute=true"), std::string::npos);
    EXPECT_NE(line.find("virtual_desk=true"), std::string::npos);
    EXPECT_NE(line.find("geometry_origin_x=-1536"), std::string::npos);
    EXPECT_NE(line.find("display_id=14c36-1"), std::string::npos);
}

TEST(PointerGeometryDiagnostics, SampleGateIsSparseAndTracksMappingChanges) {
    PointerInputSampleGate gate;
    EXPECT_EQ(gate.evaluate(1000, 11), PointerInputSampleGate::Reason::kFirstAfterChange);
    EXPECT_EQ(gate.evaluate(1500, 11), PointerInputSampleGate::Reason::kNone);
    EXPECT_EQ(gate.evaluate(3999, 11), PointerInputSampleGate::Reason::kNone);
    EXPECT_EQ(gate.evaluate(4000, 11), PointerInputSampleGate::Reason::kInterval);
    EXPECT_EQ(gate.evaluate(4100, 99), PointerInputSampleGate::Reason::kMappingInputsChanged);
    gate.note_mapping_inputs_changed();
    EXPECT_EQ(gate.evaluate(4200, 99), PointerInputSampleGate::Reason::kFirstAfterChange);
}

TEST(PointerGeometryDiagnostics, MappingSignatureChangesWithOrigin) {
    const auto left = pointer_mapping_signature(
        -1536, 0, 1920, 1080, 0, 1, 0, 0, 1920, 1080, 1, -1536, 0, 2880, 864);
    const auto physical = pointer_mapping_signature(
        -1920, 0, 1920, 1080, 0, 1, 0, 0, 1920, 1080, 1, -1920, 0, 3600, 1080);
    EXPECT_NE(left, physical);
}
