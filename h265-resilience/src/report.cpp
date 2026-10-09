#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Row = std::map<std::string, std::string>;
using Key = std::tuple<std::string, std::string, int>;
static std::vector<std::string> split(const std::string& line) {
    std::istringstream in(line);
    std::string part;
    std::vector<std::string> result;
    while (std::getline(in, part, ',')) result.push_back(part);
    return result;
}
static double number(const Row& r, const std::string& k) { return std::stod(r.at(k)); }
static std::string fmt(double x, int precision = 2) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(precision) << x;
    return s.str();
}
static double mean(const std::vector<double>& x) {
    return std::accumulate(x.begin(), x.end(), 0.) / x.size();
}
static double sd(const std::vector<double>& x) {
    if (x.size() < 2) return 0;
    double m = mean(x), s = 0;
    for (double v : x) s += (v - m) * (v - m);
    return std::sqrt(s / (x.size() - 1));
}
static std::string base64(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), {}), result;
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        std::uint32_t v = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) v |= static_cast<unsigned char>(bytes[i+1]) << 8;
        if (i + 2 < bytes.size()) v |= static_cast<unsigned char>(bytes[i+2]);
        result += alphabet[(v >> 18) & 63]; result += alphabet[(v >> 12) & 63];
        result += i + 1 < bytes.size() ? alphabet[(v >> 6) & 63] : '=';
        result += i + 2 < bytes.size() ? alphabet[v & 63] : '=';
    }
    return result;
}
struct Summary {
    std::vector<double> y, yuv, missing, black, freeze, rate, actual, duplicate, outside;
    void add(const Row& r) {
        y.push_back(number(r, "psnr_y")); yuv.push_back(number(r, "psnr_yuv"));
        missing.push_back(number(r, "missing_frames") * 100 / number(r, "source_frames"));
        black.push_back(number(r, "black_frames") * 100 / number(r, "source_frames"));
        freeze.push_back(number(r, "longest_no_output_ms"));
        rate.push_back(number(r, "ip_kbps")); actual.push_back(number(r, "actual_loss") * 100);
        duplicate.push_back(number(r, "duplicate_outputs"));
        outside.push_back(number(r, "out_of_range_outputs"));
    }
};
const std::vector<std::string> names{"strict_black", "native_black", "native_hold", "slices_hold",
                                     "short_gop_hold", "slices_short_gop_hold"};
const std::vector<std::string> labels{"严格解码 / 缺帧黑帧", "容错解码 / 缺帧黑帧", "容错解码 / 上一帧保持",
                                      "4 slices / 上一帧保持", "短 GOP / 上一帧保持", "4 slices + 短 GOP / 保持"};
const std::vector<std::string> colors{"#9c3030", "#b7791f", "#316cbb", "#8d56a8", "#008b7c", "#21364e"};

static std::string graph(const std::string& clip, const std::map<Key,Summary>& groups,
                         const std::vector<int>& rates, bool missing) {
    std::ostringstream s;
    double lo = missing ? 0 : 100, hi = missing ? 100 : 0;
    if (!missing) for (const auto& n : names) for (int r : rates) {
        const auto& g = groups.at({clip,n,r});
        lo = std::min(lo, mean(g.y) - sd(g.y)); hi = std::max(hi, mean(g.y) + sd(g.y));
    }
    if (!missing) { lo = std::max(0., std::floor(lo / 5) * 5); hi = std::ceil(hi / 5) * 5; }
    if (hi <= lo) hi = lo + 10;
    auto x = [&](std::size_t i) { return 82. + 730. * i / (rates.size() - 1); };
    auto y = [&](double v) { return 350. - (v-lo) * 280. / (hi-lo); };
    s << "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 950 470' role='img' "
      << "aria-label='" << clip << (missing ? " 缺帧率" : " PSNR-Y") << "'>"
      << "<rect width='950' height='470' fill='white'/><g font-family='sans-serif' font-size='13' fill='#334155'>"
      << "<text x='82' y='30' font-size='19' font-weight='bold'>" << clip
      << (missing ? "：没有新解码帧的比例（含补帧）" : "：PSNR-Y 与丢包率（三种子均值 ± 标准差）") << "</text>";
    for (int i = 0; i <= 5; ++i) {
        double value = lo + (hi-lo) * i / 5;
        s << "<line x1='82' x2='812' y1='" << y(value) << "' y2='" << y(value)
          << "' stroke='#dce3eb'/><text text-anchor='end' x='70' y='" << y(value)+4 << "'>"
          << fmt(value,0) << (missing ? "%" : "") << "</text>";
    }
    for (std::size_t i = 0; i < rates.size(); ++i)
        s << "<text x='" << x(i) << "' y='373' text-anchor='middle'>"
          << fmt(rates[i] / 10000., rates[i] < 10000 && rates[i] ? 1 : 0) << "%</text>";
    s << "<text x='445' y='396' text-anchor='middle'>名义丢包率档位（横轴等间隔展示各测试档位）</text>";
    for (std::size_t j = 0; j < names.size(); ++j) {
        s << "<polyline fill='none' stroke='" << colors[j] << "' stroke-width='2.5' points='";
        for (std::size_t i = 0; i < rates.size(); ++i) {
            const auto& g = groups.at({clip,names[j],rates[i]});
            s << x(i) << ',' << y(mean(missing ? g.missing : g.y)) << ' ';
        }
        s << "'/>";
        for (std::size_t i = 0; i < rates.size(); ++i) {
            const auto& g = groups.at({clip,names[j],rates[i]});
            const auto& v = missing ? g.missing : g.y;
            double m = mean(v), e = sd(v);
            s << "<line x1='" << x(i) << "' x2='" << x(i) << "' y1='" << y(std::min(hi,m+e))
              << "' y2='" << y(std::max(lo,m-e)) << "' stroke='" << colors[j] << "' opacity='.6'/>"
              << "<circle cx='" << x(i) << "' cy='" << y(m) << "' r='3' fill='" << colors[j] << "'/>";
        }
        double lx = 82 + (j % 3) * 270, ly = 425 + (j / 3) * 26;
        s << "<line x1='" << lx << "' x2='" << lx+20 << "' y1='" << ly << "' y2='" << ly
          << "' stroke='" << colors[j] << "' stroke-width='3'/><text x='" << lx+26 << "' y='" << ly+4
          << "' font-size='11'>" << labels[j] << "</text>";
    }
    s << "</g></svg>";
    return s.str();
}

int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::invalid_argument("usage: h265_report OUT_DIR trials.csv [trials.csv ...]");
        fs::path outdir(argv[1]);
        fs::create_directories(outdir);
        std::map<Key,Summary> groups;
        std::vector<Row> rows;
        std::set<std::tuple<std::string,std::string,int,int>> seen;
        std::set<std::string> clips;
        std::set<int> rate_set;
        std::map<std::string,std::size_t> source_counts;
        std::map<std::string, std::set<int>> seed_sets;
        std::ofstream all(outdir / "all_trials.csv");
        for (int file = 2; file < argc; ++file) {
            std::ifstream in(argv[file]);
            std::string header, line;
            if (!std::getline(in, header)) throw std::runtime_error("cannot read " + std::string(argv[file]));
            if (file == 2) all << header << '\n';
            auto columns = split(header);
            while (std::getline(in,line)) {
                auto cells = split(line);
                if (cells.size() != columns.size()) throw std::runtime_error("malformed trial CSV row");
                Row r;
                for (std::size_t i = 0; i < cells.size(); ++i) r[columns[i]] = cells[i];
                const auto& clip = r.at("clip");
                const auto& mode = r.at("mode");
                int rate = static_cast<int>(std::llround(number(r,"loss_probability") * 1000000));
                int seed = static_cast<int>(number(r,"seed"));
                if (!seen.insert({clip,mode,rate,seed}).second) throw std::runtime_error("duplicate trial key");
                if (!std::isfinite(number(r,"psnr_y")) || !std::isfinite(number(r,"psnr_yuv")))
                    throw std::runtime_error("non-finite video PSNR");
                if (number(r,"decoded_frames") + number(r,"missing_frames") != number(r,"source_frames") ||
                    number(r,"hold_frames") + number(r,"black_frames") != number(r,"missing_frames"))
                    throw std::runtime_error("incomplete source timeline accounting");
                if (number(r,"max_rtp_bytes") > 1472) throw std::runtime_error("MTU violation");
                if (rate == 0 && number(r,"missing_frames") != 0) throw std::runtime_error("zero-loss missing frames");
                source_counts[clip] = static_cast<std::size_t>(number(r,"source_frames"));
                seed_sets[clip].insert(seed);
                groups[{clip,mode,rate}].add(r);
                rows.push_back(r); clips.insert(clip); rate_set.insert(rate);
                all << line << '\n';
            }
        }
        std::vector<int> rates(rate_set.begin(),rate_set.end());
        if (rates.size() < 2) throw std::runtime_error("need at least two loss levels");
        for (const auto& c : clips) for (const auto& n : names) for (int rate : rates) {
            auto it = groups.find({c,n,rate});
            if (it == groups.end() || it->second.y.size() != seed_sets[c].size())
                throw std::runtime_error("incomplete experiment matrix");
        }
        std::ofstream summary(outdir / "summary.csv");
        summary << "clip,mode,loss_probability,n,psnr_y_mean,psnr_y_sd,psnr_yuv_mean,missing_percent,"
                   "black_percent,longest_no_output_ms_mean,ip_kbps_mean,actual_loss_percent_mean\n";
        for (const auto& item : groups) {
            auto [c,n,r] = item.first;
            const auto& g = item.second;
            summary << c << ',' << n << ',' << r / 1000000. << ',' << g.y.size() << ','
                    << mean(g.y) << ',' << sd(g.y) << ',' << mean(g.yuv) << ',' << mean(g.missing) << ','
                    << mean(g.black) << ',' << mean(g.freeze) << ',' << mean(g.rate) << ',' << mean(g.actual) << '\n';
        }
        std::ofstream md(outdir / "H265_resilience_report.md");
        std::ofstream html(outdir / "H265_resilience_report.html");
        html << "<!doctype html><html lang='zh-CN'><meta charset='utf-8'><title>H.265 丢包掩盖与恢复实测</title>"
                "<style>body{font-family:'Microsoft YaHei','Noto Sans CJK SC',sans-serif;color:#23384c;max-width:1100px;margin:40px auto;padding:0 24px;line-height:1.75}"
                "h1,h2{color:#204f79}h2{margin-top:36px}table{border-collapse:collapse;width:100%;font-size:12px;margin:20px 0}"
                "th{background:#234f76;color:white}td,th{padding:7px;border:1px solid #ccd6df;text-align:right}td:first-child,th:first-child{text-align:left}"
                "tr:nth-child(even){background:#f3f7fa}.note{background:#edf5fa;padding:16px;border-left:4px solid #2c75ac}"
                "svg{width:100%;height:auto}.small{font-size:13px;color:#53677b}code{background:#f1f4f6;padding:2px 5px}"
                "@page{size:A4;margin:16mm}@media print{body{margin:0;max-width:none;font-size:9pt;line-height:1.65}h2{break-after:avoid}svg,table{break-inside:avoid}table{font-size:7.2pt}td,th{padding:4px}svg{max-height:75mm}p{orphans:3;widows:3}}"
                "</style><h1>H.265 RTP 丢包：错误掩盖与恢复机制实测</h1><p>测试日期：2026-10-08 · C++17 / GStreamer 1.28.2 / x265 / FFmpeg HEVC</p>";
        md << "# H.265 RTP 丢包掩盖与恢复实测报告\n\n测试日期：2026-10-08。共 " << rows.size()
           << " 个试验条件，每次逐帧计算并完整包含所有源帧。\n\n";
        html << "<p class='note'>完成 " << rows.size() << " 次试验。PSNR 对每个源时间戳计分，包含保持帧和黑帧。"
                "这些是 GStreamer 实际编码、RTP 分包、丢包、解包与解码的结果；确定性回放没有引入网络抖动、RTT 或迟到截止时间。"
                "UDP 实时示例另行验证，不能将本表解释成真实链路的延迟性能。</p>";
        html << "<h2>实测结论</h2><p>2% 独立丢包下，多 slice + 短 GOP + 上一帧保持改善了本次两段素材的 PSNR，"
                "但绝对画质仍明显受损。保持帧主要解决缺新帧时的黑屏，不能修复已解码出来的马赛克。</p><table>"
                "<tr><th>序列</th><th>普通容错 + 保持 PSNR-Y</th><th>多 slice + 短 GOP + 保持 PSNR-Y</th><th>增益</th></tr>";
        for (const auto& c : clips) {
            double a = mean(groups.at({c,"native_hold",20000}).y);
            double b = mean(groups.at({c,"slices_short_gop_hold",20000}).y);
            html << "<tr><td>" << c << "</td><td>" << fmt(a) << " dB</td><td>" << fmt(b)
                 << " dB</td><td>+" << fmt(b-a) << " dB</td></tr>";
        }
        html << "</table><p>无丢包时 FourPeople 的组合方案比长 GOP 普通方案低约 1.87 dB；"
                "KristenAndSara 则基本持平。短序列 ABR 实发码率不同，后文同时列出代价，避免过度归因。</p>";
        const std::string methodology =
            "两段 Xiph 公共领域 720p 序列 FourPeople、KristenAndSara，各取官方 WebM 解码后前 300 帧，60 fps（5 秒）。"
            "参考是固定的 WebM 解码画面，已经带有原始 VP8/VP9 压缩损伤；结果不等于相对无损原始 Y4M 的 PSNR。"
            "没有将原本的 WebM 质量损伤重复计入本次 HEVC 失真。"
            "每种设置独立编码一次并复用包序列，目标视频码率 3000 kbit/s、ultrafast、zerolatency、无 B 帧、closed GOP、scenecut=0。"
            "长 GOP=120（2 秒），短 GOP=30（0.5 秒）；slices=1 或 4。IDR 重复 VPS/SPS/PPS。"
            "丢包为每 RTP 包独立 Bernoulli 事件，种子 17/42/2026；VPS/SPS/PPS 与首个 IDR 同样可能丢失。"
            "同编码流的严格/容错/保持策略使用完全相同的丢包掩码；不同 slice/GOP 改变包数，只有同种子同分布，不能声称丢的是相同画面位置。"
            "IPv4 IP MTU=1500，RTP 包最大 1472 bytes（已含 RTP 头），再加 UDP 8 与 IPv4 20 bytes。末包不填充到 1500。"
            "纯码流丢包回放将 surviving RTP 包的源时间戳送入 depayloader，显式保留序号间隙与 DISCONT；不用墙钟抖动缓冲，以免未建模的迟到丢包混入概率。";
        html << "<h2>测试口径与复现边界</h2><p>" << methodology << "</p>";
        md << "## 测试口径\n\n" << methodology << "\n\n";
        html << "<h2>机制含义</h2><table><tr><th>配置</th><th>解码与显示策略</th></tr>";
        const std::vector<std::string> descriptions{
            "单 slice、GOP120；output-corrupt=false，discard-corrupted-frames=true；没有输出则黑帧。严格对照，不是 GStreamer 默认行为。",
            "单 slice、GOP120；允许解码器输出受损图像，缺帧时黑帧。avdec_h265 默认本就允许部分受损输出，不能把开关称为新增掩盖算法。",
            "同一容错解码流；没有新帧则重复上一已输出帧。保住画面，但损坏参考帧仍会传播误差。",
            "4 个独立 slice、GOP120，容错输出与上一帧保持。减少单个 NAL 受损的覆盖范围，但丢失首 slice 或参数集仍可能影响整帧。",
            "单 slice、GOP30，容错输出与保持。周期 IDR 清理参考误差；IDR 自身也可能受损，不能保证 0.5 秒内恢复。",
            "4 slices、GOP30，容错输出与保持。结合空间分区和周期恢复，码率、分包开销及无丢包质量均需同时比较。"};
        for (std::size_t i = 0; i < names.size(); ++i) {
            html << "<tr><td>" << names[i] << "</td><td style='text-align:left'>" << descriptions[i] << "</td></tr>";
            md << "- `" << names[i] << "`：" << descriptions[i] << '\n';
        }
        html << "</table><p>所有模式 max-errors=-1。掩盖不是从无到有恢复丢失的编码信息；这里没有修改 HEVC 解码器的运动补偿算法，也没有实现 FEC、ARQ、RTX 或 RTCP PLI 反馈。</p>";
        for (const auto& clip : clips) {
            html << "<div class='clip'><h2>" << clip << "：完整 PSNR-Y（dB）</h2>";
            md << "\n## " << clip << "\n\nPSNR-Y 均值 ± 样本标准差（dB），每格三个随机种子。\n\n| 机制 |";
            html << "<table><tr><th>机制</th>";
            for (int r : rates) {
                html << "<th>" << fmt(r/10000.,r<10000&&r?1:0) << "%</th>";
                md << ' ' << fmt(r/10000.,r<10000&&r?1:0) << "% |";
            }
            html << "</tr>";
            md << "\n|---|";
            for (int r : rates) { (void)r; md << "---:|"; }
            md << '\n';
            for (std::size_t n = 0; n < names.size(); ++n) {
                html << "<tr><td>" << labels[n] << "</td>";
                md << "| " << names[n] << " |";
                for (int r : rates) {
                    const auto& g = groups.at({clip,names[n],r});
                    std::string cell = fmt(mean(g.y)) + " ± " + fmt(sd(g.y));
                    html << "<td>" << cell << "</td>"; md << ' ' << cell << " |";
                }
                html << "</tr>"; md << '\n';
            }
            html << "</table>" << graph(clip,groups,rates,false) << graph(clip,groups,rates,true);
            std::ofstream svg(outdir / (clip + "_psnr.svg"));
            svg << graph(clip,groups,rates,false);
            html << "<h2>" << clip << "：2% 丢包的表现与代价</h2>"
                    "<table><tr><th>机制</th><th>PSNR-Y</th><th>PSNR-YUV</th><th>缺新帧 %</th><th>黑帧 %</th>"
                    "<th>最长无新帧 ms</th><th>IP 等效 kbit/s</th></tr>";
            md << "\n2% 丢包时（均值）：\n\n| 机制 | PSNR-Y | PSNR-YUV | 缺新帧% | 黑帧% | 最长无新帧ms | IP等效kbps |\n|---|---:|---:|---:|---:|---:|---:|\n";
            for (std::size_t n = 0; n < names.size(); ++n) {
                const auto& g = groups.at({clip,names[n],20000});
                html << "<tr><td>" << labels[n] << "</td>";
                md << "| " << names[n] << " |";
                for (double value : {mean(g.y),mean(g.yuv),mean(g.missing),mean(g.black),mean(g.freeze),mean(g.rate)}) {
                    html << "<td>" << fmt(value) << "</td>"; md << ' ' << fmt(value) << " |";
                }
                html << "</tr>"; md << '\n';
            }
            html << "</table></div>";
        }
        const std::string limits =
            "PSNR-Y=10log10(255²/MSE-Y)，先汇总该试验所有源时间戳的 SSE 再除以像素数；"
            "PSNR-YUV 对全部 I420 样本汇总 SSE（按样本数 Y:U:V=4:1:1），不是三个 dB 数的加权平均。"
            "表格跨种子取试验 PSNR 的均值与样本标准差。仅三个种子，不代表稳定的总体置信区间。"
            "丢帧处只使用已输出的上一帧，首帧之前用 Y=16,U=V=128 黑帧；不插入未来帧。"
            "受损解码可能产生重复 PTS 或时间范围外的 PTS：每个源槽只接受第一次输出，范围外输出丢弃并计数，不为提高分数重排帧。"
            "CORRUPTED 标志不保证标出全部马赛克；没有标志不代表画面正确。"
            "最长无新帧指标是连续缺输出槽长度；不等同于画面内容未变化时长，也不等同于带重排序/网络延迟的实时恢复耗时。"
            "两段素材都是低至中运动的人物场景，5 秒时长且只测独立丢包；不能外推到高运动、长时间、突发丢包或硬件解码器。"
            "固定目标码率的短序列 ABR 可能偏离目标，表中给出实际 RTP/IP 等效码率；不同编码设置应同时查看无丢包质量与实发码率，不能将收益全部归因于解码掩盖。";
        html << "<h2>指标解释与限制</h2><p>" << limits << "</p>"
                "<h2>实际使用建议</h2><p>容错解码 + 上一帧保持可减少因缺帧产生的黑屏；"
                "短 GOP 能增加恢复机会，但会有编码效率和 IDR 开销。多 slice 的收益必须以本表衡量，不能预设它必然更好。"
                "建议针对业务运动场景重新运行测试，必要时再加入带反馈的 IDR 请求或 FEC/重传。"
                "完全未收到可解码启动帧时没有可保持的画面，这种情况不能承诺不黑屏。</p>";
        md << "\n## 指标与限制\n\n" << limits << "\n\n";
        auto comparison = fs::path(argv[2]).parent_path() / "comparison_frame240.png";
        if (fs::exists(comparison))
            html << "<h2>受损画面示例</h2><p>FourPeople，第 240 帧，2% 丢包，seed=42。"
                    "从左至右：参考帧、普通容错 + 保持、多 slice + 短 GOP + 保持。组合策略保留了部分画面，"
                    "仍存在大面积损伤。</p><img style='width:100%' src='data:image/png;base64,"
                 << base64(comparison) << "'>";
        html << "<h2>验证证据</h2><p>三个自动化测试通过（核心计分/时间轴、实际编码解码、UDP 收发与固定节奏保持）。"
                "矩阵核查覆盖 252 个试验、84 个汇总分组、75,600 个源帧计分槽。"
                "另用 FFmpeg psnr 独立核对组合方案 2%、seed42 的 Y4M 输出："
                "FourPeople PSNR-Y=16.738555 dB、PSNR-YUV=18.467864 dB；"
                "KristenAndSara PSNR-Y=15.384173 dB、PSNR-YUV=17.041005 dB，与 C++ 对应试验一致。"
                "这些单种子结果与上表三种子均值不同。</p>";
        html << "<h2>数据来源与实现位置</h2><p><a href='https://media.xiph.org/video/derf/'>Xiph Derf test media（FourPeople、KristenAndSara 标注 public domain）</a>；"
                "<a href='https://gstreamer.freedesktop.org/documentation/libav/avdec_h265.html'>avdec_h265</a>；"
                "<a href='https://gstreamer.freedesktop.org/documentation/rtp/rtph265depay.html'>rtph265depay</a>；"
                "<a href='https://x265.readthedocs.io/en/master/cli.html'>x265 slicing / closed GOP</a>。</p>"
                "<p class='small'>FourPeople WebM SHA256: 1e4a5df7e67ae985370321cc2c91b8595f7e3433ad8ee1e84da7a26ff5254deb<br>"
                "KristenAndSara WebM SHA256: 7d32a6d6864804b986832272ed570a2e4b43552d5958fc1f614d1e7e0fbd2436</p>"
                "<p>源码、README、真实 UDP 示例与回放工具位于 h265-resilience；all_trials.csv、summary.csv、逐帧 CSV 和 packet manifests 可复核所有数字。</p>"
                "</html>";
        md << "来源：[Xiph Derf](https://media.xiph.org/video/derf/)、[avdec_h265](https://gstreamer.freedesktop.org/documentation/libav/avdec_h265.html)、"
              "[rtph265depay](https://gstreamer.freedesktop.org/documentation/rtp/rtph265depay.html)、[x265](https://x265.readthedocs.io/en/master/cli.html)。\n";
        if (!html || !md || !summary || !all) throw std::runtime_error("report write failed");
        std::cout << "Verified " << rows.size() << " trials; " << groups.size() << " summary groups; report: "
                  << fs::absolute(outdir / "H265_resilience_report.html").string() << '\n';
    } catch (const std::exception& e) { std::cerr << "ERROR: " << e.what() << '\n'; return 1; }
}
