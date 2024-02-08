#include "MessageSinkUtils.h"
#include "TestUtils.h"
#include "poly_tail/poly_tail_config.h"
#include "read_pipeline/PolyACalculatorNode.h"
#include "utils/sequence_utils.h"

#include <catch2/catch.hpp>
#include <spdlog/spdlog.h>
#include <toml.hpp>
#include <toml/value.hpp>
#include <torch/torch.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define TEST_GROUP "[poly_a_estimator]"

namespace fs = std::filesystem;

using namespace dorado;

struct TestCase {
    int estimated_bases = 0;
    std::string test_dir;
    bool is_rna;
};

TEST_CASE("PolyACalculator: Test polyT tail estimation", TEST_GROUP) {
    auto [gt, data, is_rna] = GENERATE(
            TestCase{143, "poly_a/r9_rev_cdna", false}, TestCase{35, "poly_a/r10_fwd_cdna", false},
            TestCase{37, "poly_a/rna002", true}, TestCase{73, "poly_a/rna004", true});

    dorado::PipelineDescriptor pipeline_desc;
    std::vector<dorado::Message> messages;
    auto sink = pipeline_desc.add_node<MessageSinkToVector>({}, 100, messages);
    pipeline_desc.add_node<PolyACalculatorNode>({sink}, 2, is_rna, 1000, nullptr);

    auto pipeline = dorado::Pipeline::create(std::move(pipeline_desc), nullptr);

    fs::path data_dir = fs::path(get_data_dir(data));
    auto seq_file = data_dir / "seq.txt";
    auto signal_file = data_dir / "signal.tensor";
    auto moves_file = data_dir / "moves.bin";
    auto read = std::make_unique<SimplexRead>();
    read->read_common.seq = ReadFileIntoString(seq_file.string());
    read->read_common.qstring = std::string(read->read_common.seq.length(), '~');
    read->read_common.moves = ReadFileIntoVector(moves_file.string());
    read->read_common.model_stride = 5;
    torch::load(read->read_common.raw_data, signal_file.string());
    read->read_common.read_id = "read_id";
    // Push a Read type.
    pipeline->push_message(std::move(read));

    pipeline->terminate(DefaultFlushOptions());

    CHECK(messages.size() == 1);

    auto out = std::get<SimplexReadPtr>(std::move(messages[0]));
    CHECK(out->read_common.rna_poly_tail_length == gt);
}

TEST_CASE("PolyACalculator: Test polyT tail estimation with custom config", TEST_GROUP) {
    auto config = (fs::path(get_data_dir("poly_a/configs")) / "polya.toml").string();

    dorado::PipelineDescriptor pipeline_desc;
    std::vector<dorado::Message> messages;
    auto sink = pipeline_desc.add_node<MessageSinkToVector>({}, 100, messages);
    pipeline_desc.add_node<PolyACalculatorNode>({sink}, 2, false, 1000, &config);

    auto pipeline = dorado::Pipeline::create(std::move(pipeline_desc), nullptr);

    fs::path data_dir = fs::path(get_data_dir("poly_a/r9_rev_cdna"));
    auto seq_file = data_dir / "seq.txt";
    auto signal_file = data_dir / "signal.tensor";
    auto moves_file = data_dir / "moves.bin";
    auto read = std::make_unique<SimplexRead>();
    read->read_common.seq = ReadFileIntoString(seq_file.string());
    read->read_common.qstring = std::string(read->read_common.seq.length(), '~');
    read->read_common.moves = ReadFileIntoVector(moves_file.string());
    read->read_common.model_stride = 5;
    torch::load(read->read_common.raw_data, signal_file.string());
    read->read_common.read_id = "read_id";
    // Push a Read type.
    pipeline->push_message(std::move(read));

    pipeline->terminate(DefaultFlushOptions());

    CHECK(messages.size() == 1);

    auto out = std::get<SimplexReadPtr>(std::move(messages[0]));
    CHECK(out->read_common.rna_poly_tail_length == -1);
}

TEST_CASE("PolyTailConfig: Test parsing file", TEST_GROUP) {
    auto tmp_dir = TempDir(fs::temp_directory_path() / "polya_test");
    std::filesystem::create_directories(tmp_dir.m_path);

    SECTION("Only one primer is provided") {
        auto path = (tmp_dir.m_path / "only_one_primer.toml").string();
        const toml::value data{{"anchors", toml::table{{"front_primer", "ACTG"}}}};
        const std::string fmt = toml::format(data);
        std::ofstream outfile(path);
        if (outfile.is_open()) {
            outfile << fmt;
            outfile.close();
        }

        CHECK_THROWS_WITH(dorado::poly_tail::prepare_config(&path),
                          "Both front_primer and rear_primer must be provided in the PolyA "
                          "configuration file.");
    }

    SECTION("Only one plasmid flank is provided") {
        auto path = (tmp_dir.m_path / "only_one_flank.toml").string();
        const toml::value data{{"anchors", toml::table{{"plasmid_rear_flank", "ACTG"}}}};
        const std::string fmt = toml::format(data);
        std::ofstream outfile(path);
        if (outfile.is_open()) {
            outfile << fmt;
            outfile.close();
        }

        CHECK_THROWS_WITH(dorado::poly_tail::prepare_config(&path),
                          "Both plasmid_front_flank and plasmid_rear_flank must be provided in the "
                          "PolyA configuration file.");
    }

    SECTION("Parse all supported configs") {
        auto path = (tmp_dir.m_path / "only_one_flank.toml").string();
        const toml::value data{{"anchors", toml::table{{"plasmid_front_flank", "CGTA"},
                                                       {"plasmid_rear_flank", "ACTG"},
                                                       {"front_primer", "AAAAAA"},
                                                       {"rear_primer", "GGGGGG"}}},
                               {"tail", toml::table{{"tail_interrupt_length", 10}}}};
        const std::string fmt = toml::format(data);
        std::ofstream outfile(path);
        if (outfile.is_open()) {
            outfile << fmt;
            outfile.close();
        }

        auto config = dorado::poly_tail::prepare_config(&path);
        CHECK(config.front_primer == "AAAAAA");
        CHECK(config.rc_front_primer == "TTTTTT");
        CHECK(config.rear_primer == "GGGGGG");
        CHECK(config.rc_rear_primer == "CCCCCC");
        CHECK(config.plasmid_front_flank == "CGTA");
        CHECK(config.rc_plasmid_front_flank == "TACG");
        CHECK(config.plasmid_rear_flank == "ACTG");
        CHECK(config.rc_plasmid_rear_flank == "CAGT");
        CHECK(config.is_plasmid);  // Since the plasmid flanks were specified
        CHECK(config.tail_interrupt_length == 10);
    }
}
