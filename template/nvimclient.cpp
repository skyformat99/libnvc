/*
 * =====================================================================================
 *
 *       Filename: nvimclient.cpp
 *        Created: 01/08/2019 05:33:48
 *    Description: 
 *
 *        Version: 1.0
 *       Revision: none
 *       Compiler: gcc
 *
 *         Author: ANHONG
 *          Email: anhonghe@gmail.com
 *   Organization: USTC
 *
 * =====================================================================================
 */

#include <utf8.h>
#include <cinttypes>
#include "libnvc.hpp"
#include "strfunc.hpp"

static int measure_utf8_column_width(uint32_t utf8_code)
{
    // check stackoverflow:
    // https://stackoverflow.com/questions/5117393/number-of-character-cells-used-by-string
    std::setlocale(LC_ALL, "");

    char buf[8];
    std::memset(buf, 0, 8);
    std::memcpy(buf, &utf8_code, 4);

    wchar_t wcbuf[2];
    if(std::mbstowcs(wcbuf, buf, 2) != 1){
        throw std::runtime_error(str_fflprintf(": Failed to convert to wide char for column meansurement"));
    }
    return wcwidth(wcbuf[0]);
}

static size_t peek_utf8_code(const char *str, size_t length, uint32_t *pcode)
{
    if(str == nullptr){
        throw std::invalid_argument(str_fflprintf(": Invalid argument: (nullptr)"));
    }

    if(length == 0){
        length = std::strlen(str);
    }

    if(length == 0){
        throw std::invalid_argument(str_fflprintf(": Invalid argument: empty string"));
    }

    auto pbegin = str;
    auto pend   = pbegin;

    try{
        utf8::advance(pend, 1, str + length);
    }catch(...){
        throw std::invalid_argument(str_fflprintf(": Invalid argument: failed to peek the first utf8 code"));
    }

    size_t advanced = pend - pbegin;
    if(advanced > 4){
        throw std::runtime_error(str_fflprintf(": First utf8 code from \"%s\" is longer than 4 bytes: %zu", str, advanced));
    }

    if(pcode){
        *pcode = (uint32_t)(0);
        std::memcpy(pcode, pbegin, pend - pbegin);
    }
    return advanced;
}

libnvc::CELL::CELL(uint32_t utf8, int hl_id)
{
    clear();
    if(utf8 != INVALID_UTF8){
        set(utf8, hl_id);
    }
}

void libnvc::CELL::set(uint32_t utf8, int hl_id)
{
    m_utf8code = utf8;
    m_hlid     = hl_id;

    switch(auto col_width = measure_utf8_column_width(m_utf8code)){
        case 0:
        case 1:
        case 2:
            {
                m_wcwidth = col_width;
                return;
            }
        default:
            {
                throw std::invalid_argument(str_fflprintf(": Invalid utf8 code: %" PRIu32, m_utf8code));
            }
    }
}

libnvc::nvim_client::nvim_client(libnvc::io_device *pdevice, size_t width, size_t height)
    : libnvc::api_client(pdevice)
    , m_hldefs()
    , m_options()
    , m_currboard(std::make_unique<libnvc::board>(width, height))
    , m_backboard()
{}

void libnvc::nvim_client::on_flush()
{
    m_backboard = m_currboard->clone();
    for(size_t y = 0; y < height(); ++y){
        std::string line;
        for(size_t x = 0; x < width(); ++x){
            if(const char *chr = m_backboard->get_cell(x, y).len4_cstr()){
                line += chr;
            }else{
                // found an invalid cell
                // warning, could be a bug of nvim
            }
        }
        libnvc::log(LOG_INFO, line.c_str());
    }
}

void libnvc::nvim_client::on_grid_cursor_goto(int64_t, int64_t row, int64_t col)
{
    m_currboard->set_cursor(col, row);
}

size_t libnvc::nvim_client::set_cell(size_t x, size_t y, const std::string &str, int64_t hl_id, int repeat)
{
    uint32_t utf8_code = 0;
    peek_utf8_code(str.data(), str.length(), &utf8_code);

    m_currboard->get_cell(x, y).set(utf8_code, hl_id);
    switch(auto column_width = m_currboard->get_cell(x, y).wc_width()){
        case 1:
            {
                for(int64_t pos = 0; pos < repeat; ++pos){
                    m_currboard->get_cell(x + pos, y).set(utf8_code, hl_id);
                }
                return repeat;
            }
        case 2:
            {
                if(repeat != 1){
                    throw std::runtime_error(str_fflprintf(": Protocol error: wide utf8 char get column width: %d", column_width));
                }
                m_currboard->get_cell(x + 1, y).clear();
                return 2;
            }
        case 0:
            {
                return 2;
            }
        default:
            {
                throw std::runtime_error(str_fflprintf(": Invalid utf8 column width: %d", column_width));
            }
    }
}

void libnvc::nvim_client::on_grid_line(int64_t, int64_t row, int64_t col_start, const std::vector<libnvc::object> & data)
{
    size_t x = col_start;
    size_t y = row;

    // check doc of grid_line
    // each cell contains [text(, hl_id, repeat)]
    int64_t hl_id = 0;

    for(size_t index = 0; index < data.size(); ++index){
        int64_t repeat = 1;
        std::string utf8char = "";
        switch(auto &vec_cell = std::get<std::vector<libnvc::object_wrapper>>(data[index]); vec_cell.size()){
            case 3:
                {
                    repeat = std::get<int64_t>(vec_cell[2].ref());
                    [[fallthrough]];
                }
            case 2:
                {
                    hl_id = std::get<int64_t>(vec_cell[1].ref());
                    [[fallthrough]];
                }
            case 1:
                {
                    utf8char = std::get<std::string>(vec_cell[0].ref());
                    break;
                }
            default:
                {
                    throw std::runtime_error(str_fflprintf(": Invalid array for cell (%zu, %zu)", x, y));
                }
        }
        x += set_cell(x, y, utf8char, hl_id, repeat);
    }
}

void libnvc::nvim_client::on_hl_attr_define(int64_t id, const std::map<std::string, libnvc::object> &rgb_attrs, const std::map<std::string, libnvc::object> &, const std::vector<libnvc::object> &)
{
    hl_attrdef this_attrdef;
    for(auto &elem: rgb_attrs){
        if(elem.first == "foreground"){
            this_attrdef.color_fg = std::get<int64_t>(elem.second) & 0x0000000000ffffff;
            this_attrdef.color_fg_defined = 1;
            continue;
        }

        if(elem.first == "background"){
            this_attrdef.color_bg = std::get<int64_t>(elem.second) & 0x0000000000ffffff;
            this_attrdef.color_bg_defined = 1;
            continue;
        }

        if(elem.first == "special"){
            this_attrdef.color_sp = std::get<int64_t>(elem.second) & 0x0000000000ffffff;
            this_attrdef.color_sp_defined = 1;
            continue;
        }

        if(elem.first == "reverse"){
            this_attrdef.reverse = 1;
            continue;
        }

        if(elem.first == "italic"){
            this_attrdef.italic = 1;
            continue;
        }

        if(elem.first == "bold"){
            this_attrdef.bold = 1;
            continue;
        }

        if(elem.first == "underline"){
            this_attrdef.underline = 1;
            continue;
        }

        if(elem.first == "undercurl"){
            this_attrdef.undercurl = 1;
            continue;
        }
    }

    m_hldefs.resize(id + 1);
    m_hldefs[id] = this_attrdef;
}
