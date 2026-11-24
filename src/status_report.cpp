#include "status_report.hpp"

#include <charconv>
#include <iomanip>
#include <limits>

namespace status_report {
namespace {
    struct Field {
        const char* key;
        const char* label;
        bool bytes = false;
    };

    std::vector<Field> fields(uint8_t flags) {
        std::vector<Field> result;
        if (flags & STATUS_FLAG_MEMORY) {
            result.insert(result.end(), {
                {"memory_configured", "Configurada", true},
                {"memory_reserved", "Reservada", true},
                {"memory_pages_total", "Paginas"},
                {"memory_pages_used", "Usadas"},
                {"memory_pages_free", "Libres"}
            });
        }
        if (flags & STATUS_FLAG_PAGES) {
            result.insert(result.end(), {
                {"pages_total", "Copias"}, {"pages_state_invalid", "I"},
                {"pages_state_shared", "S"}, {"pages_state_modified", "M"}
            });
        }
        if (flags & STATUS_FLAG_STATS) {
            result.insert(result.end(), {
                {"pages_owned", "Propias"}, {"pages_shared", "Compartidas"},
                {"page_faults", "Fallos"}, {"coalesced_faults", "Deduplicados"},
                {"page_requests", "Solicitudes"}, {"page_transfers", "Transferencias"},
                {"bytes_sent", "Enviados", true}, {"bytes_received", "Recibidos", true},
                {"invalidations_sent", "Inv. enviadas"}, {"invalidations_received", "Inv. recibidas"}
            });
        }
        return result;
    }

    bool parse(NodeReport& report, const std::vector<Field>& expected) {
        std::map<std::string, uint64_t> values;
        std::istringstream input(report.text);
        std::string line;
        while (std::getline(input, line)) {
            const auto equals = line.find('=');
            if (equals == std::string::npos) continue;
            const auto key = line.substr(0, equals);
            if (key != "node_id" && std::none_of(expected.begin(), expected.end(),
                    [&](const Field& field) { return key == field.key; })) continue;
            const auto text = line.substr(equals + 1);
            uint64_t value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                !values.emplace(key, value).second) {
                report.error = "campo STATUS invalido o repetido: " + key;
                return false;
            }
        }
        const auto id = values.find("node_id");
        if (id == values.end() || id->second != report.node.id) {
            report.error = "node_id no corresponde al participante consultado";
            return false;
        }
        values.erase(id);
        for (const auto& field : expected) {
            if (!values.count(field.key)) {
                report.error = std::string("STATUS incompleto: falta ") + field.key;
                return false;
            }
        }
        if (values.count("memory_pages_total") &&
            (values.at("memory_pages_used") > values.at("memory_pages_total") ||
             values.at("memory_pages_free") != values.at("memory_pages_total") - values.at("memory_pages_used"))) {
            report.error = "contadores de memoria inconsistentes";
            return false;
        }
        report.values = std::move(values);
        return true;
    }

    std::string value_text(uint64_t value, bool bytes) {
        if (!bytes) return std::to_string(value);
        std::ostringstream out;
        if (value >= 1024 * 1024) out << value / (1024.0 * 1024.0) << " MB";
        else if (value >= 1024) out << value / 1024.0 << " KB";
        else out << value << " B";
        return out.str();
    }

    void table(std::ostream& out, const GlobalReport& report, uint8_t flag) {
        const auto columns = fields(flag);
        std::vector<std::vector<std::string>> rows;
        std::vector<std::string> header{"Nombre"};
        for (const auto& column : columns) header.push_back(column.label);
        rows.push_back(header);
        for (const auto& node : report.nodes) {
            if (!node.error.empty()) continue;
            std::vector<std::string> row{node.node.node_name};
            for (const auto& column : columns) row.push_back(value_text(node.values.at(column.key), column.bytes));
            rows.push_back(std::move(row));
        }
        std::vector<std::string> total{"TOTAL"};
        for (const auto& column : columns) total.push_back(value_text(report.totals.at(column.key), column.bytes));
        rows.push_back(std::move(total));
        std::vector<std::size_t> widths(header.size());
        for (const auto& row : rows) {
            for (std::size_t i = 0; i < row.size(); ++i) widths[i] = std::max(widths[i], row[i].size());
        }
        for (const auto& row : rows) {
            for (std::size_t i = 0; i < row.size(); ++i) {
                out << (i == 0 ? std::left : std::right)
                    << std::setw(static_cast<int>(widths[i])) << row[i] << "  ";
            }
            out << '\n';
        }
    }
}

GlobalReport collect(const std::vector<NodeInfo>& nodes, uint8_t flags, const StatusQuery& query) {
    GlobalReport result;
    const auto expected = fields(flags);
    for (const auto& field : expected) result.totals.emplace(field.key, 0);
    for (const auto& node : nodes) {
        NodeReport report;
        report.node = node;
        report.text = query(node, flags, report.error);
        if (report.text.empty() && report.error.empty()) report.error = "STATUS sin respuesta";
        if (report.error.empty() && parse(report, expected)) {
            for (const auto& value : report.values) {
                if (value.second > std::numeric_limits<uint64_t>::max() - result.totals.at(value.first)) {
                    report.error = "desbordamiento al sumar " + value.first;
                    break;
                }
            }
            if (report.error.empty()) {
                for (const auto& value : report.values) result.totals.at(value.first) += value.second;
                ++result.responded;
            }
        }
        result.nodes.push_back(std::move(report));
    }
    return result;
}

std::string global(const GlobalReport& report, uint8_t flags) {
    std::ostringstream out;
    out << "Consulta global: " << (report.partial() ? "PARCIAL" : "COMPLETA")
        << " (" << report.responded << "/" << report.nodes.size() << " nodos)\n"
        << "global_nodes_known=" << report.nodes.size() << '\n'
        << "global_nodes_responded=" << report.responded << '\n'
        << "global_nodes_unavailable=" << report.nodes.size() - report.responded << '\n'
        << "global_partial=" << (report.partial() ? "si" : "no") << '\n';
    std::size_t name_width = 6, address_width = 9;
    for (const auto& report_node : report.nodes) {
        const auto& node = report_node.node;
        name_width = std::max(name_width, node.node_name.size());
        address_width = std::max(address_width, node.ip_address.size() + 1 + std::to_string(node.tcp_port).size());
    }
    out << std::right << std::setw(20) << "ID" << "  "
        << std::left << std::setw(static_cast<int>(name_width)) << "Nombre" << "  "
        << std::setw(static_cast<int>(address_width)) << "Direccion" << "  Estado\n";
    for (const auto& node : report.nodes) {
        out << std::right << std::setw(20) << node.node.id << "  "
            << std::left << std::setw(static_cast<int>(name_width)) << node.node.node_name << "  "
            << std::setw(static_cast<int>(address_width))
            << node.node.ip_address + ":" + std::to_string(node.node.tcp_port) << "  "
            << (node.error.empty() ? "OK" : "NO CONSULTADO") << '\n';
    }
    if (flags & STATUS_FLAG_MEMORY) {
        out << "\nMemoria por nodo\n";
        table(out, report, STATUS_FLAG_MEMORY);
    }
    if (flags & STATUS_FLAG_PAGES) {
        out << "\nCopias locales de paginas por nodo\n";
        table(out, report, STATUS_FLAG_PAGES);
    }
    if (flags & STATUS_FLAG_STATS) {
        out << "\nEstadisticas por nodo\n";
        table(out, report, STATUS_FLAG_STATS);
    }
    for (const auto& node : report.nodes) {
        if (!node.error.empty()) {
            out << "No consultado: " << node.node.node_name << " (" << node.node.id << ") "
                << node.node.ip_address << ":" << node.node.tcp_port << ": " << node.error << '\n';
            continue;
        }
        if ((flags & (STATUS_FLAG_PAGES | STATUS_FLAG_HISTORY)) == 0) continue;
        out << "\nDetalle de " << node.node.node_name << " (" << node.node.id << ")\n";
        std::istringstream input(node.text);
        std::string line;
        while (std::getline(input, line)) {
            if (((flags & STATUS_FLAG_PAGES) && line.compare(0, 5, "page=") == 0) ||
                ((flags & STATUS_FLAG_HISTORY) && line.compare(0, 8, "history_") == 0) ||
                ((flags & STATUS_FLAG_HISTORY) && line.compare(0, 8, "history=") == 0)) out << line << '\n';
        }
    }
    if (!report.totals.empty()) out << "\nTotales globales (suma de valores locales)\n";
    for (const auto& field : fields(flags)) out << "total_" << field.key << '=' << report.totals.at(field.key) << '\n';
    return out.str();
}
}
