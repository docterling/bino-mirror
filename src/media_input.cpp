/*
 * This file is part of bino, a 3D video player.
 *
 * Copyright (C) 2010-2011
 * Martin Lambers <marlam@marlam.de>
 * Frédéric Devernay <frederic.devernay@inrialpes.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "dbg.h"
#include "exc.h"
#include "msg.h"
#include "str.h"

#include "media_input.h"


media_input::media_input() :
    _active_video_stream(-1), _active_audio_stream(-1),
    _initial_skip(0), _duration(-1)
{
}

media_input::~media_input()
{
}

void media_input::get_video_stream(int stream, int &media_object, int &media_object_video_stream) const
{
    assert(stream < video_streams());

    for (size_t i = 0; i < _media_objects.size(); i++)
    {
        if (_media_objects[i].video_streams() < stream + 1)
        {
            stream -= _media_objects[i].video_streams();
            continue;
        }
        media_object = i;
        media_object_video_stream = stream;
        break;
    }
}

void media_input::get_audio_stream(int stream, int &media_object, int &media_object_audio_stream) const
{
    assert(stream < audio_streams());

    for (size_t i = 0; i < _media_objects.size(); i++)
    {
        if (_media_objects[i].audio_streams() < stream + 1)
        {
            stream -= _media_objects[i].audio_streams();
            continue;
        }
        media_object = i;
        media_object_audio_stream = stream;
        break;
    }
}

// Get the basename of an URL (just the file name, without leading paths)
static std::string basename(const std::string &url)
{
    size_t last_slash = url.find_last_of('/');
    size_t last_backslash = url.find_last_of('\\');
    size_t i = std::min(last_slash, last_backslash);
    if (last_slash != std::string::npos && last_backslash != std::string::npos)
    {
        i = std::max(last_slash, last_backslash);
    }
    if (i == std::string::npos)
    {
        return url;
    }
    else
    {
        return url.substr(i + 1);
    }
}

void media_input::open(const std::vector<std::string> &urls)
{
    assert(urls.size() > 0);

    // Open media objects
    _media_objects.resize(urls.size());
    for (size_t i = 0; i < urls.size(); i++)
    {
        _media_objects[i].open(urls[i]);
    }

    // Construct id for this input
    _id = basename(_media_objects[0].url());
    for (size_t i = 1; i < _media_objects.size(); i++)
    {
        _id += '/';
        _id += basename(_media_objects[i].url());
    }

    // Gather metadata
    for (size_t i = 0; i < _media_objects.size(); i++)
    {
        std::string pfx = (_media_objects.size() == 1 ? "" : str::from(i + 1) + " - ");
        for (size_t j = 0; j < _media_objects[i].tags(); j++)
        {
            _tag_names.push_back(pfx + _media_objects[i].tag_name(j));
            _tag_values.push_back(pfx + _media_objects[i].tag_value(j));
        }
    }

    // Gather streams and stream names
    for (size_t i = 0; i < _media_objects.size(); i++)
    {
        std::string pfx = (_media_objects.size() == 1 ? "" : str::from(i + 1) + " - ");
        for (int j = 0; j < _media_objects[i].video_streams(); j++)
        {
            std::string pfx2 = (_media_objects[i].video_streams() == 1 ? "" : str::from(j + 1) + " - ");
            _video_stream_names.push_back(pfx + pfx2
                    + _media_objects[i].video_frame_template(j).format_info());
        }
    }
    for (size_t i = 0; i < _media_objects.size(); i++)
    {
        std::string pfx = (_media_objects.size() == 1 ? "" : str::from(i + 1) + " - ");
        for (int j = 0; j < _media_objects[i].audio_streams(); j++)
        {
            std::string pfx2 = (_media_objects[i].audio_streams() == 1 ? "" : str::from(j + 1) + " - ");
            _audio_stream_names.push_back(pfx + pfx2
                    + _media_objects[i].audio_blob_template(j).format_info());
        }
    }

    // Set duration information
    _duration = std::numeric_limits<int64_t>::max();
    for (size_t i = 0; i < _media_objects.size(); i++)
    {
        for (int j = 0; j < _media_objects[i].video_streams(); j++)
        {
            int64_t d = _media_objects[i].video_duration(j);
            if (d < _duration)
            {
                _duration = d;
            }
        }
        for (int j = 0; j < _media_objects[i].audio_streams(); j++)
        {
            int64_t d = _media_objects[i].audio_duration(j);
            if (d < _duration)
            {
                _duration = d;
            }
        }
    }
 
    // Skip advertisement in 3dtv.at movies. Only works for single media objects.
    try { _initial_skip = str::to<int64_t>(tag_value("StereoscopicSkip")); } catch (...) { }

    // Find stereo layout and set active video stream(s)
    _supports_stereo_layout_separate = false;
    if (video_streams() == 2)
    {
        int o0, o1, v0, v1;
        get_video_stream(0, o0, v0);
        get_video_stream(1, o1, v1);
        video_frame t0 = _media_objects[o0].video_frame_template(v0);
        video_frame t1 = _media_objects[o1].video_frame_template(v1);
        if (t0.width == t1.width
                && t0.height == t1.height
                && (t0.aspect_ratio <= t1.aspect_ratio && t0.aspect_ratio >= t1.aspect_ratio)
                && t0.layout == t1.layout
                && t0.color_space == t1.color_space
                && t0.value_range == t1.value_range
                && t0.chroma_location == t1.chroma_location)
        {
            _supports_stereo_layout_separate = true;
        }
    }
    if (_supports_stereo_layout_separate)
    {
        _active_video_stream = 0;
        int o, s;
        get_video_stream(_active_video_stream, o, s);
        _video_frame = _media_objects[o].video_frame_template(s);
        _video_frame.stereo_layout = video_frame::separate;
    }
    else if (video_streams() > 0)
    {
        _active_video_stream = 0;
        int o, s;
        get_video_stream(_active_video_stream, o, s);
        _video_frame = _media_objects[o].video_frame_template(s);
    }
    else
    {
        _active_video_stream = -1;
    }
    if (_active_video_stream >= 0)
    {
        select_video_stream(_active_video_stream);
    }

    // Set active audio stream
    _active_audio_stream = (audio_streams() > 0 ? 0 : -1);
    if (_active_audio_stream >= 0)
    {
        int o, s;
        get_audio_stream(_active_audio_stream, o, s);
        _audio_blob = _media_objects[o].audio_blob_template(s);
        select_audio_stream(_active_audio_stream);
    }

    // Print summary
    msg::inf("Input:");
    msg::inf("    Duration: %g seconds", duration() / 1e6f);
    msg::inf("    Stereo layout: %s", video_frame::stereo_layout_to_string(
                video_frame_template().stereo_layout, video_frame_template().stereo_layout_swap).c_str());
    for (int i = 0; i < video_streams(); i++)
    {
        int o, s;
        get_video_stream(i, o, s);
        msg::inf("    Video %s: %s", video_stream_name(i).c_str(),
                _media_objects[o].video_frame_template(s).format_name().c_str());
    }
    if (video_streams() == 0)
    {
        msg::inf("    No video.");
    }
    for (int i = 0; i < audio_streams(); i++)
    {
        int o, s;
        get_audio_stream(i, o, s);
        msg::inf("    Audio %s: %s", audio_stream_name(i).c_str(), 
                _media_objects[o].audio_blob_template(s).format_name().c_str());
    }
    if (audio_streams() == 0)
    {
        msg::inf("    No audio.");
    }
}

const std::string &media_input::id() const
{
    return _id;
}

size_t media_input::tags() const
{
    return _tag_names.size();
}

const std::string &media_input::tag_name(size_t i) const
{
    assert(_tag_names.size() > i);
    return _tag_names[i];
}

const std::string &media_input::tag_value(size_t i) const
{
    assert(_tag_values.size() > i);
    return _tag_values[i];
}

const std::string &media_input::tag_value(const std::string &tag_name) const
{
    static std::string empty;
    for (size_t i = 0; i < _tag_names.size(); i++)
    {
        if (std::string(tag_name) == _tag_names[i])
        {
            return _tag_values[i];
        }
    }
    return empty;
}

const video_frame &media_input::video_frame_template() const
{
    assert(_active_video_stream >= 0);
    return _video_frame;
}

int media_input::video_frame_rate_numerator() const
{
    assert(_active_video_stream >= 0);
    int o, s;
    get_video_stream(_active_video_stream, o, s);
    return _media_objects[s].video_frame_rate_numerator(s);
}

int media_input::video_frame_rate_denominator() const
{
    assert(_active_video_stream >= 0);
    int o, s;
    get_video_stream(_active_video_stream, o, s);
    return _media_objects[s].video_frame_rate_denominator(s);
}

int64_t media_input::video_frame_duration() const
{
    assert(_active_video_stream >= 0);
    return static_cast<int64_t>(video_frame_rate_denominator()) * 1000000 / video_frame_rate_numerator();
}

const audio_blob &media_input::audio_blob_template() const
{
    assert(_active_audio_stream >= 0);
    return _audio_blob;
}

bool media_input::stereo_layout_is_supported(video_frame::stereo_layout_t layout, bool) const
{
    if (video_streams() < 1)
    {
        return false;
    }
    assert(_active_video_stream >= 0);
    assert(_active_video_stream < video_streams());
    int o, s;
    get_video_stream(_active_video_stream, o, s);
    const video_frame &t = _media_objects[o].video_frame_template(s);
    bool supported = true;
    if (((layout == video_frame::left_right || layout == video_frame::left_right_half) && t.raw_width % 2 != 0)
            || ((layout == video_frame::top_bottom || layout == video_frame::top_bottom_half) && t.raw_height % 2 != 0)
            || (layout == video_frame::even_odd_rows && t.raw_height % 2 != 0)
            || (layout == video_frame::separate && !_supports_stereo_layout_separate))
    {
        supported = false;
    }
    return supported;
}

void media_input::set_stereo_layout(video_frame::stereo_layout_t layout, bool swap)
{
    assert(stereo_layout_is_supported(layout, swap));
    int o, s;
    get_video_stream(_active_video_stream, o, s);
    const video_frame &t = _media_objects[o].video_frame_template(s);
    _video_frame = t;
    _video_frame.stereo_layout = layout;
    _video_frame.stereo_layout_swap = swap;
    _video_frame.set_view_dimensions();
    if (layout == video_frame::separate)
    {
        // If we switched the layout to 'separate', then we have to seek to the
        // position of the first video stream, or else the second video stream
        // is out of sync.
        int64_t pos = _media_objects[o].tell();
        if (pos > std::numeric_limits<int64_t>::min())
        {
            seek(pos);
        }
    }
}

void media_input::select_video_stream(int video_stream)
{
    assert(video_stream >= 0);
    assert(video_stream < video_streams());
    if (_video_frame.stereo_layout == video_frame::separate)
    {
        for (size_t i = 0; i < _media_objects.size(); i++)
        {
            for (int j = 0; j < _media_objects[i].video_streams(); j++)
            {
                _media_objects[i].video_stream_set_active(j, true);
            }
        }
    }
    else
    {
        video_frame::stereo_layout_t layout = _video_frame.stereo_layout;
        bool swap = _video_frame.stereo_layout_swap;
        _active_video_stream = video_stream;
        set_stereo_layout(layout, swap);
        int o, s;
        get_video_stream(_active_video_stream, o, s);
        for (size_t i = 0; i < _media_objects.size(); i++)
        {
            for (int j = 0; j < _media_objects[i].video_streams(); j++)
            {
                _media_objects[i].video_stream_set_active(j, (i == static_cast<size_t>(o) && j == s));
            }
        }
    }
}

void media_input::select_audio_stream(int audio_stream)
{
    assert(audio_stream >= 0);
    assert(audio_stream < audio_streams());
    _active_audio_stream = audio_stream;
    int o, s;
    get_audio_stream(_active_audio_stream, o, s);
    for (size_t i = 0; i < _media_objects.size(); i++)
    {
        for (int j = 0; j < _media_objects[i].audio_streams(); j++)
        {
            _media_objects[i].audio_stream_set_active(j, (i == static_cast<size_t>(o) && j == s));
        }
    }
}

void media_input::start_video_frame_read()
{
    assert(_active_video_stream >= 0);
    if (_video_frame.stereo_layout == video_frame::separate)
    {
        int o0, s0, o1, s1;
        get_video_stream(0, o0, s0);
        get_video_stream(1, o1, s1);
        _media_objects[o0].start_video_frame_read(s0);
        _media_objects[o1].start_video_frame_read(s1);
    }
    else
    {
        int o, s;
        get_video_stream(_active_video_stream, o, s);
        _media_objects[o].start_video_frame_read(s);
    }
}

video_frame media_input::finish_video_frame_read()
{
    assert(_active_video_stream >= 0);
    if (_video_frame.stereo_layout == video_frame::separate)
    {
        int o0, s0, o1, s1;
        get_video_stream(0, o0, s0);
        get_video_stream(1, o1, s1);
        video_frame f0 = _media_objects[o0].finish_video_frame_read(s0);
        video_frame f1 = _media_objects[o1].finish_video_frame_read(s1);
        if (!f0.is_valid() || !f1.is_valid())
        {
            return video_frame();
        }
        video_frame frame = _video_frame;
        for (int p = 0; p < 3; p++)
        {
            frame.data[0][p] = f0.data[0][p];
            frame.data[1][p] = f1.data[0][p];
            frame.line_size[0][p] = f0.line_size[0][p];
            frame.line_size[1][p] = f1.line_size[0][p];
        }
        frame.presentation_time = f0.presentation_time;
        return frame;
    }
    else
    {
        int o, s;
        get_video_stream(_active_video_stream, o, s);
        video_frame f = _media_objects[o].finish_video_frame_read(s);
        if (!f.is_valid())
        {
            return video_frame();
        }
        video_frame frame = _video_frame;
        for (int p = 0; p < 3; p++)
        {
            frame.data[0][p] = f.data[0][p];
            frame.line_size[0][p] = f.line_size[0][p];
        }
        frame.presentation_time = f.presentation_time;
        return frame;
    }
}

void media_input::start_audio_blob_read(size_t size)
{
    assert(_active_audio_stream >= 0);
    int o, s;
    get_audio_stream(_active_audio_stream, o, s);
    _media_objects[o].start_audio_blob_read(s, size);
}

audio_blob media_input::finish_audio_blob_read()
{
    assert(_active_audio_stream >= 0);
    int o, s;
    get_audio_stream(_active_audio_stream, o, s);
    return _media_objects[o].finish_audio_blob_read(s);
}

void media_input::seek(int64_t pos)
{
    for (size_t i = 0; i < _media_objects.size(); i++)
    {
        _media_objects[i].seek(pos);
    }
}

void media_input::close()
{
    _id = "";
    for (size_t i = 0; i < _media_objects.size(); i++)
    {
        _media_objects[i].close();
    }
    _media_objects.clear();
    _tag_names.clear();
    _tag_values.clear();
    _video_stream_names.clear();
    _audio_stream_names.clear();
    _active_video_stream = -1;
    _active_audio_stream = -1;
    _initial_skip = 0;
    _duration = -1;
    _video_frame = video_frame();
    _audio_blob = audio_blob();
}