// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ekphore/player.h"
#include "ekphore/ui.h"
#include "offline_engine_service.h"

#include <sndfile.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

struct MidiEvent {
    std::size_t frame;
    std::string type;
    std::uint8_t key;
    std::uint8_t value;
};

struct Options {
    std::filesystem::path instrument;
    std::optional<std::filesystem::path> render_path;
    std::optional<std::filesystem::path> midi_events_path;
    std::vector<MidiEvent> midi_events;
    std::optional<std::filesystem::path> trace_audio_path;
    std::optional<std::size_t> render_block_size;
    std::optional<std::uint32_t> external_modulation_random_state;
    std::optional<std::string> dump_ui_asset;
    std::optional<std::filesystem::path> dump_ui_asset_path;
    std::uint8_t note{ 60 };
    std::uint8_t velocity{ 100 };
    std::vector<std::pair<std::uint8_t, std::uint8_t>> control_changes;
    std::uint32_t sample_rate{ 44100 };
    double seconds{ 2.0 };
    std::optional<double> note_seconds;
    bool dump_scripts{};
    bool dump_voices{};
    bool dump_engine_calls{};
    bool dump_ksp_events{};
    bool dump_effects{};
    bool dump_modulators{};
    bool dump_ui_controls{};
    bool dump_native_ui_hits{};
    bool dump_group_outputs{};
    std::vector<std::string> dump_native_ui_modules;
    std::vector<std::pair<std::size_t, std::string>> dump_script_values;
    std::vector<std::tuple<std::size_t, std::string, std::int64_t>>
            script_control_overrides;
    std::vector<std::pair<std::string, double>> native_control_overrides;
};

auto parse_options(int argc, char **argv) -> Options {
    if (argc < 2)
        throw std::invalid_argument(
                "usage: ekphore INSTRUMENT [--render OUTPUT.wav] [--note N] "
                "[--velocity V] [--sample-rate HZ] [--seconds S] "
                "[--note-seconds S] "
                "[--midi-events FILE (frame note_on|note_off|cc key value)] "
                "[--cc CONTROLLER VALUE] "
                "[--set-script-control SLOT NAME VALUE] "
                "[--trace-audio OUTPUT.jsonl] [--render-block-size FRAMES] "
                "[--external-modulation-rng-state UINT32 (diagnostic replay)] "
                "[--dump-ui-asset NAME OUTPUT] "
                "[--dump-scripts] [--dump-voices] [--dump-engine-calls] "
                "[--dump-ksp-events] [--dump-script-value SLOT NAME] "
                "[--dump-effects] [--dump-modulators] [--dump-group-outputs]\n"
                "--dump-script-value prints before/after render. With "
                "--dump-ksp-events, "
                "also watches that slot/name at positive play_note calls (max 64 "
                "names, "
                "16 array elements, 256 bytes/string). Diagnostic only, not "
                "RT-safe: "
                "Runtime::get may copy full arrays; output bounds do not bound "
                "allocation.");
    Options options;
    options.instrument = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        static constexpr std::array<
                std::pair<std::string_view, bool Options::*>, 9>
                flags{ {
                        { "--dump-effects", &Options::dump_effects },
                        { "--dump-engine-calls", &Options::dump_engine_calls },
                        { "--dump-group-outputs",
                                &Options::dump_group_outputs },
                        { "--dump-ksp-events", &Options::dump_ksp_events },
                        { "--dump-modulators", &Options::dump_modulators },
                        { "--dump-native-ui-hits",
                                &Options::dump_native_ui_hits },
                        { "--dump-scripts", &Options::dump_scripts },
                        { "--dump-ui-controls", &Options::dump_ui_controls },
                        { "--dump-voices", &Options::dump_voices },
                } };
        const auto flag = std::ranges::find(flags, option,
                &std::pair<std::string_view, bool Options::*>::first);
        if (flag != flags.end()) {
            options.*(flag->second) = true;
            continue;
        }
        if (option == "--dump-native-ui-module") {
            if (index + 1 >= argc)
                throw std::invalid_argument(
                        "--dump-native-ui-module requires NAME");
            options.dump_native_ui_modules.emplace_back(argv[++index]);
            continue;
        }
        if (option == "--dump-script-value") {
            if (index + 2 >= argc)
                throw std::invalid_argument(
                        "--dump-script-value requires SLOT and NAME");
            const auto slot = std::stoull(argv[++index]);
            options.dump_script_values.emplace_back(slot, argv[++index]);
            continue;
        }
        if (option == "--set-script-control") {
            if (index + 3 >= argc)
                throw std::invalid_argument(
                        "--set-script-control requires SLOT, NAME, and VALUE");
            const auto slot = std::stoull(argv[++index]);
            std::string name = argv[++index];
            const auto control_value = std::stoll(argv[++index]);
            options.script_control_overrides.emplace_back(
                    slot, std::move(name), control_value);
            continue;
        }
        if (option == "--set-native-control") {
            if (index + 2 >= argc)
                throw std::invalid_argument(
                        "--set-native-control requires NAME and VALUE");
            std::string name = argv[++index];
            const auto value = std::stod(argv[++index]);
            options.native_control_overrides.emplace_back(
                    std::move(name), value);
            continue;
        }
        if (option == "--dump-ui-asset") {
            if (index + 2 >= argc)
                throw std::invalid_argument(
                        "--dump-ui-asset requires NAME and OUTPUT");
            options.dump_ui_asset = argv[++index];
            options.dump_ui_asset_path = argv[++index];
            continue;
        }
        if (option == "--cc") {
            if (index + 2 >= argc)
                throw std::invalid_argument(
                        "--cc requires CONTROLLER and VALUE");
            const auto controller = std::stoi(argv[++index]);
            const auto value = std::stoi(argv[++index]);
            if (controller < 0 || controller > 127 || value < 0 || value > 127)
                throw std::invalid_argument(
                        "--cc controller and value must be in [0,127]");
            options.control_changes.emplace_back(
                    static_cast<std::uint8_t>(controller),
                    static_cast<std::uint8_t>(value));
            continue;
        }
        if (index + 1 >= argc)
            throw std::invalid_argument("missing value for " + option);
        const std::string value = argv[++index];
        if (option == "--render") {
            options.render_path = value;
        } else if (option == "--midi-events") {
            options.midi_events_path = value;
        } else if (option == "--trace-audio") {
            options.trace_audio_path = value;
        } else if (option == "--external-modulation-rng-state") {
            if (value.empty() || !std::ranges::all_of(value, [](char c) {
                    return c >= '0' && c <= '9';
                }))
                throw std::invalid_argument(
                        "external modulation RNG state must be a decimal uint32");
            const auto parsed = std::stoull(value);
            if (parsed > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument(
                        "external modulation RNG state exceeds uint32");
            options.external_modulation_random_state =
                    static_cast<std::uint32_t>(parsed);
        } else if (option == "--render-block-size") {
            if (value.empty() || !std::ranges::all_of(value, [](char c) {
                    return c >= '0' && c <= '9';
                }))
                throw std::invalid_argument(
                        "render block size must be a positive integer");
            const auto parsed = std::stoull(value);
            if (parsed == 0 || parsed > std::numeric_limits<std::size_t>::max())
                throw std::invalid_argument("invalid render block size");
            options.render_block_size = static_cast<std::size_t>(parsed);
        } else if (option == "--note") {
            const auto parsed = std::stoi(value);
            if (parsed < 0 || parsed > 127)
                throw std::invalid_argument("note must be in [0,127]");
            options.note = static_cast<std::uint8_t>(parsed);
        } else if (option == "--velocity") {
            const auto parsed = std::stoi(value);
            if (parsed < 0 || parsed > 127)
                throw std::invalid_argument("velocity must be in [0,127]");
            options.velocity = static_cast<std::uint8_t>(parsed);
        } else if (option == "--sample-rate") {
            const auto parsed = std::stoul(value);
            if (parsed == 0 || parsed > 768000)
                throw std::invalid_argument("invalid sample rate");
            options.sample_rate = static_cast<std::uint32_t>(parsed);
        } else if (option == "--seconds") {
            options.seconds = std::stod(value);
            if (!(options.seconds > 0.0) || options.seconds > 3600.0)
                throw std::invalid_argument("seconds must be in (0,3600]");
        } else if (option == "--note-seconds") {
            options.note_seconds = std::stod(value);
            if (!(*options.note_seconds > 0.0) ||
                    *options.note_seconds > 3600.0)
                throw std::invalid_argument("note-seconds must be in (0,3600]");
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    if (options.midi_events_path) {
        std::ifstream input(*options.midi_events_path);
        if (!input)
            throw std::invalid_argument("cannot open MIDI event file");
        const auto frames = static_cast<std::int64_t>(
                options.seconds * options.sample_rate);
        std::string line;
        while (std::getline(input, line)) {
            line = line.substr(0, line.find('#'));
            if (line.find_first_not_of(" \t\r") == std::string::npos)
                continue;
            std::istringstream row(line);
            std::int64_t frame;
            int key, value;
            std::string type, extra;
            if (!(row >> frame >> type >> key >> value) || (row >> extra) ||
                    frame < 0 || frame >= frames || key < 0 || key > 127 ||
                    value < 0 || value > 127 ||
                    (type != "note_on" && type != "note_off" && type != "cc") ||
                    (!options.midi_events.empty() &&
                            frame < static_cast<std::int64_t>(
                                            options.midi_events.back().frame)))
                throw std::invalid_argument(
                        "invalid or unordered MIDI event: " + line);
            options.midi_events.push_back({ static_cast<std::size_t>(frame),
                    type, static_cast<std::uint8_t>(key),
                    static_cast<std::uint8_t>(value) });
        }
        if (options.midi_events.empty())
            throw std::invalid_argument("empty MIDI event file");
    }
    return options;
}

auto write_wave(const std::filesystem::path &path,
        const ekphore::AudioBlock &audio, std::uint32_t sample_rate) -> void {
    SF_INFO info{};
    info.samplerate = static_cast<int>(sample_rate);
    info.channels = 2;
    info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    SNDFILE *file = sf_open(path.string().c_str(), SFM_WRITE, &info);
    if (!file)
        throw std::runtime_error(sf_strerror(nullptr));
    std::vector<float> interleaved(audio.left.size() * 2U);
    for (std::size_t frame = 0; frame < audio.left.size(); ++frame) {
        interleaved[frame * 2U] = audio.left[frame];
        interleaved[frame * 2U + 1U] = audio.right[frame];
    }
    const auto written = sf_writef_float(file, interleaved.data(),
            static_cast<sf_count_t>(audio.left.size()));
    const auto close_result = sf_close(file);
    if (written != static_cast<sf_count_t>(audio.left.size()) ||
            close_result != 0)
        throw std::runtime_error("failed to write complete WAV render");
}

class EmptyUiAssets final : public ekphore::UiAssetProvider {
  public:
    [[nodiscard]] std::shared_ptr<const ekphore::UiImage> image(
            std::string_view) const override {
        return {};
    }
};

} // namespace

int main(int argc, char **argv) {
    try {
        const auto options = parse_options(argc, argv);
        ekphore::Player player;
        player.enable_engine_call_trace(options.dump_engine_calls);
        player.enable_ksp_event_trace(options.dump_ksp_events);
        const auto report = player.load(options.instrument);
        if (options.external_modulation_random_state) {
            if (!options.render_path || !report.ok() ||
                    !player.restore_external_modulation_random_state(
                            *options.external_modulation_random_state))
                throw std::invalid_argument("RNG replay requires an offline "
                                            "render with no active voices");
            std::cout << "diagnostic-external-modulation-rng-state="
                      << *options.external_modulation_random_state << '\n';
        }
        if (options.dump_ksp_events)
            player.set_ksp_event_trace_watches(options.dump_script_values);
        std::cout << report.instrument_name << "\n"
                  << "programs=" << report.programs
                  << " groups=" << report.groups << " zones=" << report.zones
                  << " samples=" << report.samples
                  << " scripts=" << report.scripts
                  << " insert-fx=" << report.program_insert_effects
                  << " send-fx=" << report.program_send_effects
                  << " main-fx=" << report.program_main_effects
                  << " buses=" << report.insert_buses
                  << " bus-fx=" << report.insert_bus_effects << '\n';
        for (const auto &diagnostic : report.diagnostics)
            std::cerr << diagnostic << '\n';
        if (!report.ok())
            return 1;
        for (const auto &[slot, name, value] :
                options.script_control_overrides) {
            if (!player.set_script_control(slot, name, value))
                throw std::runtime_error("script control override failed: " +
                                         std::to_string(slot) + ":" + name);
        }
        for (const auto &[name, value] : options.native_control_overrides) {
            if (!player.set_native_ui_control(name, value))
                throw std::runtime_error(
                        "native control override failed: " + name);
        }
        if (options.dump_effects) {
            for (const auto &effect : report.effect_summary)
                std::cout << "effect=" << effect << '\n';
        }
        if (options.dump_modulators) {
            for (const auto &modulator : report.modulation_summary)
                std::cout << "modulator=" << modulator << '\n';
        }
        if (options.dump_engine_calls) {
            for (const auto &call : player.engine_call_trace())
                std::cout << "engine-call=" << call << '\n';
        }
        if (options.dump_group_outputs) {
            const auto state = player.persistent_state();
            for (std::size_t index = 0; index < state.group_outputs.size();
                    ++index)
                std::cout << "group-output=" << index << ':'
                          << state.group_outputs[index] << '\n';
        }
        const auto dump_script_values = [&](const char *prefix) {
            for (const auto &[slot, name] : options.dump_script_values) {
                std::cout << prefix << slot << ':' << name << '=';
                const auto value = player.script_runtime_value(slot, name);
                if (!value) {
                    std::cout << "<missing>\n";
                    continue;
                }
                std::visit(
                        [&](const auto &typed) {
                            if constexpr (requires { typed.size(); }) {
                                std::cout << '[';
                                for (std::size_t index = 0;
                                        index < typed.size(); ++index) {
                                    if (index)
                                        std::cout << ',';
                                    std::cout << typed[index];
                                }
                                std::cout << ']';
                            } else {
                                std::cout << typed;
                            }
                        },
                        *value);
                std::cout << '\n';
            }
        };
        dump_script_values("script-value=");
        if (options.dump_scripts) {
            const auto scripts = player.script_sources();
            const auto persistent = player.script_persistent_variables();
            for (std::size_t slot = 0; slot < scripts.size(); ++slot) {
                std::cout << "--- script " << slot << " persistent="
                          << (slot < persistent.size() ? persistent[slot].size()
                                                       : 0)
                          << " ---\n"
                          << scripts[slot] << '\n';
                if (slot < persistent.size()) {
                    for (const auto &variable : persistent[slot])
                        std::cout << "persistent "
                                  << variable.name.value_or("<unnamed>") << '='
                                  << variable.value.value_or(
                                             variable.serialized_value)
                                  << '\n';
                }
            }
        }
        for (const auto &module : options.dump_native_ui_modules) {
            std::cout << "--- native ui module " << module << " ---\n";
            if (const auto source = player.native_ui_module_source(module))
                std::cout << *source << '\n';
            else
                std::cout << "<not found>\n";
        }
        if (options.dump_ui_controls) {
            const auto document = player.ui_document();
            std::cout << "ui-document=" << document.width << 'x'
                      << document.height << " performance-slot=";
            if (document.performance_view_script_slot)
                std::cout << *document.performance_view_script_slot;
            else
                std::cout << '-';
            std::cout << " wallpaper=" << document.background_picture
                      << " wallpaper-offset=" << document.background_offset_y
                      << " wallpaper-skin-offsets=";
            std::cout << " tabs=";
            for (const auto &[slot, title] : document.performance_view_tabs)
                std::cout << '[' << slot << ':' << title << ']';
            for (std::size_t index = 0;
                    index < document.background_skin_offsets.size(); ++index) {
                if (index != 0)
                    std::cout << ',';
                std::cout << document.background_skin_offsets[index];
            }
            std::cout << " native-ui=" << document.native_ui_module
                      << " native-ui-root-type="
                      << document.native_ui_root_value_type
                      << " native-ui-materialized-type="
                      << document.native_ui_materialized_value_type
                      << " native-ui-materialized-components="
                      << document.native_ui_materialized_component_count
                      << '\n';
            if (!document.native_ui_materialized_component_kinds.empty()) {
                std::cout << "native-ui-materialized-kinds=";
                for (std::size_t index = 0;
                        index <
                        document.native_ui_materialized_component_kinds.size();
                        ++index) {
                    if (index)
                        std::cout << ',';
                    std::cout << document.native_ui_materialized_component_kinds
                                         [index];
                }
                std::cout << '\n';
            }
            for (const auto &root : document.native_ui_component_roots)
                std::cout << "native-ui-root=" << root.kind
                          << " properties=" << root.properties.size()
                          << " modifiers=" << root.modifiers.size()
                          << " children=" << root.children.size() << '\n';
            const auto dump_native_node =
                    [&](const auto &self,
                            const ekphore::NativeUiComponentNode &node,
                            const std::size_t depth) -> void {
                std::cout << "native-ui-node=" << depth << ':' << node.kind;
                if (!node.role.empty())
                    std::cout << " role=" << node.role;
                for (const auto &property : node.properties) {
                    std::cout << ' ' << property.name << '=';
                    std::visit([](const auto &value) { std::cout << value; },
                            property.value);
                }
                for (const auto &modifier : node.modifiers) {
                    std::cout << " ." << modifier.name << '(';
                    for (std::size_t index = 0;
                            index < modifier.arguments.size(); ++index) {
                        if (index)
                            std::cout << ',';
                        const auto &argument = modifier.arguments[index];
                        std::cout << argument.name << '=';
                        std::visit(
                                [](const auto &value) { std::cout << value; },
                                argument.value);
                    }
                    for (std::size_t index = 0; index < modifier.actions.size();
                            ++index) {
                        if (!modifier.arguments.empty() || index)
                            std::cout << ',';
                        const auto &action = modifier.actions[index];
                        std::cout << action.phase << "@" << action.id;
                    }
                    std::cout << ')';
                }
                std::cout << '\n';
                for (const auto &child : node.children)
                    self(self, child, depth + 1);
            };
            for (const auto &root : document.native_ui_component_roots)
                dump_native_node(dump_native_node, root, 0);
            if (!document.native_ui_product_modules.empty()) {
                std::cout << "native-ui-product-modules=";
                for (std::size_t index = 0;
                        index < document.native_ui_product_modules.size();
                        ++index) {
                    if (index)
                        std::cout << ',';
                    std::cout << document.native_ui_product_modules[index];
                }
                std::cout << '\n';
            }
            if (!document.native_ui_external_modules.empty()) {
                std::cout << "native-ui-external-modules=";
                for (std::size_t index = 0;
                        index < document.native_ui_external_modules.size();
                        ++index) {
                    if (index)
                        std::cout << ',';
                    std::cout << document.native_ui_external_modules[index];
                }
                std::cout << '\n';
            }
            if (!document.native_ui_declarative_components.empty()) {
                std::cout << "native-ui-components=";
                for (std::size_t index = 0;
                        index <
                        document.native_ui_declarative_components.size();
                        ++index) {
                    if (index)
                        std::cout << ',';
                    std::cout
                            << document.native_ui_declarative_components[index];
                }
                std::cout << '\n';
            }
            if (!document.native_ui_method_calls.empty()) {
                std::cout << "native-ui-methods=";
                for (std::size_t index = 0;
                        index < document.native_ui_method_calls.size();
                        ++index) {
                    if (index)
                        std::cout << ',';
                    std::cout << document.native_ui_method_calls[index];
                }
                std::cout << '\n';
            }
            for (const auto &diagnostic : document.native_ui_diagnostics)
                std::cout << "native-ui-diagnostic=" << diagnostic << '\n';
            for (const auto &control : document.controls) {
                std::cout << "ui-control=" << control.name
                          << " kind=" << static_cast<int>(control.kind)
                          << " text=" << control.text
                          << " label=" << control.label
                          << " bounds=" << control.bounds.x << ','
                          << control.bounds.y << ',' << control.bounds.width
                          << ',' << control.bounds.height
                          << " range=" << control.minimum << ','
                          << control.maximum << ',' << control.step
                          << " value=";
                if (const auto *value =
                                std::get_if<std::int64_t>(&control.value))
                    std::cout << *value;
                else
                    std::cout << '?';
                std::cout
                        << " font=" << control.font_type << '/'
                        << control.font_type_on << '/'
                        << control.font_type_off_hover << '/'
                        << control.font_type_on_hover << '/'
                        << control.font_type_off_pressed << '/'
                        << control.font_type_on_pressed
                        << " font-name=" << control.font_name << '/'
                        << control.font_name_on << '/'
                        << control.font_name_off_hover << '/'
                        << control.font_name_on_hover << '/'
                        << control.font_name_off_pressed << '/'
                        << control.font_name_on_pressed << " text-color="
                        << static_cast<int>(control.text_color.red) << ','
                        << static_cast<int>(control.text_color.green) << ','
                        << static_cast<int>(control.text_color.blue) << ','
                        << static_cast<int>(control.text_color.alpha)
                        << " table-colors="
                        << static_cast<int>(control.table_background_color.red)
                        << ','
                        << static_cast<int>(
                                   control.table_background_color.green)
                        << ','
                        << static_cast<int>(control.table_background_color.blue)
                        << ','
                        << static_cast<int>(
                                   control.table_background_color.alpha)
                        << '/' << static_cast<int>(control.table_bar_color.red)
                        << ','
                        << static_cast<int>(control.table_bar_color.green)
                        << ',' << static_cast<int>(control.table_bar_color.blue)
                        << ','
                        << static_cast<int>(control.table_bar_color.alpha)
                        << '/' << static_cast<int>(control.table_zero_color.red)
                        << ','
                        << static_cast<int>(control.table_zero_color.green)
                        << ','
                        << static_cast<int>(control.table_zero_color.blue)
                        << ','
                        << static_cast<int>(control.table_zero_color.alpha)
                        << " picture=" << control.picture
                        << " slot=" << control.script_slot
                        << " parent=" << control.parent_name
                        << " hidden=" << control.hidden
                        << " enabled=" << control.enabled
                        << " authored=" << control.authored << '\n';
            }
        }
        if (options.dump_native_ui_hits) {
            const auto document = player.ui_document();
            const EmptyUiAssets assets;
            std::vector<ekphore::NativeUiHitRegion> hits;
            (void)ekphore::UiRenderer{ &assets }.render(document, 1.0F, &hits);
            for (const auto &hit : hits) {
                std::cout << "native-ui-hit=" << hit.x << ',' << hit.y << ','
                          << hit.width << ',' << hit.height;
                if (!hit.control_id.empty())
                    std::cout << " control=" << hit.control_id;
                for (const auto &action : hit.actions)
                    std::cout << ' ' << action.phase << '@' << action.id;
                std::cout << '\n';
            }
        }
        if (options.dump_ui_asset) {
            const auto bytes = player.ui_asset_bytes(*options.dump_ui_asset);
            if (!bytes)
                throw std::runtime_error(
                        "UI asset not found: " + *options.dump_ui_asset);
            std::ofstream output(*options.dump_ui_asset_path,
                    std::ios::binary | std::ios::trunc);
            if (!output)
                throw std::runtime_error("cannot create UI asset output: " +
                                         options.dump_ui_asset_path->string());
            output.write(reinterpret_cast<const char *>(bytes->data()),
                    static_cast<std::streamsize>(bytes->size()));
            if (!output)
                throw std::runtime_error("failed to write complete UI asset");
        }
        if (options.render_path) {
            player.enable_audio_trace(options.trace_audio_path.has_value());
            for (const auto &[controller, value] : options.control_changes)
                player.control_change(controller, value);
            const auto note =
                    options.midi_events_path
                            ? ekphore::NoteResult{}
                            : player.note_on(options.note, options.velocity);
            for (const auto &diagnostic : note.diagnostics)
                std::cerr << diagnostic << '\n';
            // a script can play the note after a wait(), so no voices yet is
            // fine
            if (options.dump_voices) {
                for (const auto &voice : player.active_voices()) {
                    std::cout
                            << "voice=" << voice.voice_id
                            << " event=" << voice.event_id
                            << " zone=" << voice.zone_index << " group="
                            << (voice.group_index ? std::to_string(
                                                            *voice.group_index)
                                                  : "-")
                            << " sample=" << voice.sample_index
                            << " note=" << static_cast<unsigned>(voice.note)
                            << " velocity="
                            << static_cast<unsigned>(voice.velocity)
                            << " pitch-ratio=" << voice.pitch_ratio
                            << " downstream-tune=" << voice.downstream_tune
                            << " sample-rate=" << voice.source_sample_rate
                            << " sample-root=" << voice.source_root_note
                            << " sample-tune=" << voice.sample_tune
                            << " zone-root=" << voice.zone_root_note
                            << " zone-tune=" << voice.zone_tune
                            << " group-tune=" << voice.group_tune
                            << " program-tune=" << voice.program_tune
                            << " key-track=" << voice.key_tracking
                            << " interpolation="
                            << static_cast<unsigned>(
                                       voice.interpolation_quality)
                            << " gain=" << voice.gain
                            << " group-gain=" << voice.group_gain
                            << " program-gain=" << voice.program_gain
                            << " sample-start="
                            << voice.sample_start.value_or(0)
                            << " sample-end=" << voice.sample_end.value_or(0)
                            << '\n';
                }
            }
            const auto frames = static_cast<std::size_t>(
                    options.seconds * static_cast<double>(options.sample_rate));
            std::size_t render_frame{};
            const auto render_span = [&](std::size_t span_frames) {
                // keep the host grid even without tracing, MIDI gets checked
                // on it before each block
                const auto host_frames =
                        options.render_block_size.value_or(512);
                ekphore::AudioBlock result;
                result.left.reserve(span_frames);
                result.right.reserve(span_frames);
                // an event can split a span but don't restart the grid there
                for (std::size_t offset = 0; offset < span_frames;) {
                    ekphore::service_offline_engine_commands(player);
                    const auto count = std::min<std::size_t>(
                            host_frames - render_frame % host_frames,
                            span_frames - offset);
                    auto block = player.render(count, options.sample_rate);
                    ekphore::service_offline_engine_commands(player);
                    render_frame += count;
                    offset += count;
                    result.offline_sample_data_incomplete |=
                            block.offline_sample_data_incomplete;
                    result.left.insert(result.left.end(), block.left.begin(),
                            block.left.end());
                    result.right.insert(result.right.end(), block.right.begin(),
                            block.right.end());
                }
                return result;
            };
            if (options.midi_events_path) {
                for (const auto &event : options.midi_events) {
                    auto kind = ekphore::Player::InputKind::note_on;
                    if (event.type == "cc")
                        kind = ekphore::Player::InputKind::control_change;
                    else if (event.type == "note_off" || event.value == 0)
                        kind = ekphore::Player::InputKind::note_off;
                    player.enqueue_input(
                            { kind, event.frame, event.key, event.value });
                }
            } else if (options.note_seconds &&
                       *options.note_seconds < options.seconds) {
                const auto note_frames = static_cast<std::size_t>(
                        *options.note_seconds *
                        static_cast<double>(options.sample_rate));
                player.enqueue_input({ ekphore::Player::InputKind::note_off,
                        note_frames, options.note, 0 });
            }
            auto audio = render_span(frames);
            if (audio.offline_sample_data_incomplete) {
                if (options.dump_engine_calls)
                    for (const auto &call : player.engine_call_trace())
                        std::cout << "engine-call-after-render=" << call
                                  << '\n';
                throw std::runtime_error(
                        "offline render failed: required sample data is "
                        "unavailable or source processing is unsupported");
            }
            if (options.trace_audio_path) {
                std::ofstream trace(*options.trace_audio_path, std::ios::trunc);
                if (!trace)
                    throw std::runtime_error("cannot open audio trace output");
                // don't let decimal rounding hide the difference from Kontakt
                trace << std::setprecision(
                        std::numeric_limits<double>::max_digits10);
                for (const auto &entry : player.audio_trace()) {
                    trace << "{\"sequence\":" << entry.sequence
                          << ",\"stage\":\"" << entry.stage
                          << "\",\"program\":" << entry.program_index;
                    if (entry.voice_id)
                        trace << ",\"voice_id\":" << *entry.voice_id;
                    if (entry.event_id)
                        trace << ",\"event_id\":" << *entry.event_id;
                    if (entry.group_index)
                        trace << ",\"group\":" << *entry.group_index;
                    if (entry.bus_index)
                        trace << ",\"bus\":" << *entry.bus_index;
                    if (entry.effect_slot)
                        trace << ",\"effect_slot\":" << *entry.effect_slot;
                    if (entry.sample_index)
                        trace << ",\"sample_index\":" << *entry.sample_index;
                    if (entry.sample_path) {
                        // hex so the UTF-8 survives, read it back with
                        // bytes.fromhex()
                        constexpr char digits[] = "0123456789abcdef";
                        trace << ",\"sample_path_utf8_hex\":\"";
                        for (const unsigned char byte : *entry.sample_path)
                            trace << digits[byte >> 4] << digits[byte & 15];
                        trace << '"';
                    }
                    if (entry.sample_serialized_path) {
                        // a monolith resource isn't a real filename, keep the
                        // numbers too
                        constexpr char digits[] = "0123456789abcdef";
                        if (entry.sample_serialized_path->kontakt_path_type)
                            trace << ",\"sample_path_type\":"
                                  << *entry.sample_serialized_path
                                              ->kontakt_path_type;
                        trace << ",\"sample_resolution_base\":"
                              << static_cast<int>(entry.sample_serialized_path
                                                 ->resolution_base);
                        trace << ",\"sample_serialized_components\":[";
                        bool first = true;
                        for (const auto &component :
                                entry.sample_serialized_path->segments) {
                            if (!first)
                                trace << ',';
                            first = false;
                            trace << "{\"kind\":" << component.kind
                                  << ",\"serialized_kind\":"
                                  << static_cast<int>(component.serialized_kind)
                                  << ",\"payload_kind\":"
                                  << static_cast<int>(component.payload_kind);
                            if (component.text) {
                                trace << ",\"text_utf8_hex\":\"";
                                for (const unsigned char byte : *component.text)
                                    trace << digits[byte >> 4]
                                          << digits[byte & 15];
                                trace << '"';
                            }
                            if (component.value)
                                trace << ",\"value\":" << *component.value;
                            trace << '}';
                        }
                        trace << ']';
                    }
                    trace << ",\"frames\":" << entry.frames
                          << ",\"frame_start\":" << entry.frame_start
                          << ",\"peak_left\":" << entry.peak_left
                          << ",\"peak_right\":" << entry.peak_right
                          << ",\"rms_left\":" << entry.rms_left
                          << ",\"rms_right\":" << entry.rms_right
                          << ",\"sample_hash\":" << entry.sample_hash;
                    const auto write_preview = [&](std::string_view name,
                                                       const auto &values) {
                        trace << ",\"" << name << "\":[";
                        for (std::size_t index = 0; index < values.size();
                                ++index) {
                            if (index != 0)
                                trace << ',';
                            trace << values[index];
                        }
                        trace << ']';
                    };
                    write_preview("preview_left", entry.preview_left);
                    write_preview("preview_right", entry.preview_right);
                    trace << "}\n";
                }
                if (!trace)
                    throw std::runtime_error("failed to write audio trace");
            }
            write_wave(*options.render_path, audio, options.sample_rate);
            const auto absolute_peak = [](const std::vector<float> &channel) {
                float peak = 0.0F;
                for (const float sample : channel)
                    peak = std::max(peak, std::abs(sample));
                return peak;
            };
            dump_script_values("script-value-after-render=");
            if (options.dump_engine_calls) {
                for (const auto &call : player.engine_call_trace())
                    std::cout << "engine-call-after-render=" << call << '\n';
            }
            std::cout << "rendered=" << frames
                      << " rate=" << options.sample_rate
                      << " note=" << static_cast<unsigned>(options.note)
                      << " velocity=" << static_cast<unsigned>(options.velocity)
                      << " voices=" << note.voice_ids.size()
                      << " peak-left=" << absolute_peak(audio.left)
                      << " peak-right=" << absolute_peak(audio.right)
                      << " output=" << options.render_path->string() << '\n';
            if (options.dump_ksp_events) {
                for (const auto &event : player.ksp_event_trace())
                    std::cout << "ksp-event=" << event << '\n';
            }
        }
        return 0;
    } catch (const std::invalid_argument &error) {
        std::cerr << "ekphore: " << error.what() << '\n';
        return 2;
    } catch (const std::exception &error) {
        std::cerr << "ekphore: " << error.what() << '\n';
        return 1;
    }
}
