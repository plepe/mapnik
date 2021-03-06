/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2012 Artem Pavlenko
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#include "geojson_datasource.hpp"
#include "geojson_featureset.hpp"

#include <fstream>
#include <algorithm>

// boost
#include <boost/variant.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/spirit/include/support_multi_pass.hpp>

#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/geometries.hpp>
#include <boost/geometry.hpp>
#include <boost/geometry/extensions/index/rtree/rtree.hpp>

// mapnik
#include <mapnik/unicode.hpp>
#include <mapnik/utils.hpp>
#include <mapnik/feature.hpp>
#include <mapnik/feature_kv_iterator.hpp>
#include <mapnik/value_types.hpp>
#include <mapnik/box2d.hpp>
#include <mapnik/debug.hpp>
#include <mapnik/proj_transform.hpp>
#include <mapnik/projection.hpp>
#include <mapnik/util/geometry_to_ds_type.hpp>
#include <mapnik/json/feature_collection_grammar.hpp>
#include <mapnik/json/feature_collection_grammar_impl.hpp>
#include <mapnik/json/feature_grammar_impl.hpp>
#include <mapnik/json/geometry_grammar_impl.hpp>
#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/support_multi_pass.hpp>

using mapnik::datasource;
using mapnik::parameters;

DATASOURCE_PLUGIN(geojson_datasource)

struct attr_value_converter : public boost::static_visitor<mapnik::eAttributeType>
{
    mapnik::eAttributeType operator() (mapnik::value_integer /*val*/) const
    {
        return mapnik::Integer;
    }

    mapnik::eAttributeType operator() (double /*val*/) const
    {
        return mapnik::Double;
    }

    mapnik::eAttributeType operator() (float /*val*/) const
    {
        return mapnik::Double;
    }

    mapnik::eAttributeType operator() (bool /*val*/) const
    {
        return mapnik::Boolean;
    }

    mapnik::eAttributeType operator() (std::string const& /*val*/) const
    {
        return mapnik::String;
    }

    mapnik::eAttributeType operator() (mapnik::value_unicode_string const& /*val*/) const
    {
        return mapnik::String;
    }

    mapnik::eAttributeType operator() (mapnik::value_null const& /*val*/) const
    {
        return mapnik::String;
    }
};

geojson_datasource::geojson_datasource(parameters const& params)
  : datasource(params),
    type_(datasource::Vector),
    desc_(geojson_datasource::name(),
          *params.get<std::string>("encoding","utf-8")),
    filename_(),
    inline_string_(),
    extent_(),
    features_(),
    tree_(16,1)
{
    boost::optional<std::string> inline_string = params.get<std::string>("inline");
    if (inline_string)
    {
        inline_string_ = *inline_string;
    }
    else
    {
        boost::optional<std::string> file = params.get<std::string>("file");
        if (!file) throw mapnik::datasource_exception("GeoJSON Plugin: missing <file> parameter");

        boost::optional<std::string> base = params.get<std::string>("base");
        if (base)
            filename_ = *base + "/" + *file;
        else
            filename_ = *file;
    }
    if (!inline_string_.empty())
    {
        std::istringstream in(inline_string_);
        parse_geojson(in);
    }
    else
    {
#if defined (_WINDOWS)
        std::ifstream in(mapnik::utf8_to_utf16(filename_),std::ios_base::in | std::ios_base::binary);
#else
        std::ifstream in(filename_.c_str(),std::ios_base::in | std::ios_base::binary);
#endif
        if (!in.is_open())
        {
            throw mapnik::datasource_exception("GeoJSON Plugin: could not open: '" + filename_ + "'");
        }
        parse_geojson(in);
        in.close();
    }
}

template <typename T>
void geojson_datasource::parse_geojson(T & stream)
{
    using base_iterator_type = std::istreambuf_iterator<char>;
    boost::spirit::multi_pass<base_iterator_type> begin =
        boost::spirit::make_default_multi_pass(base_iterator_type(stream));

    boost::spirit::multi_pass<base_iterator_type> end =
        boost::spirit::make_default_multi_pass(base_iterator_type());

    // FIXME - for perf we need to declare grammar as 'static const'
    // but we cannot because then all features will interact only with the first context_ptr
    // created in the process which leads to very odd bugs
    mapnik::context_ptr ctx = std::make_shared<mapnik::context_type>();
    mapnik::transcoder tr("utf8");
    mapnik::json::feature_collection_grammar<boost::spirit::multi_pass<base_iterator_type>,mapnik::feature_impl> fc_grammar(ctx, tr);
    boost::spirit::standard_wide::space_type space;
    bool result = boost::spirit::qi::phrase_parse(begin, end, fc_grammar, space, features_);
    if (!result)
    {
        if (!inline_string_.empty()) throw mapnik::datasource_exception("geojson_datasource: Failed parse GeoJSON file from in-memory string");
        else throw mapnik::datasource_exception("geojson_datasource: Failed parse GeoJSON file '" + filename_ + "'");
    }

    std::size_t count=0;
    for (mapnik::feature_ptr const& f : features_)
    {
        mapnik::box2d<double> box = f->envelope();
        if (count == 0)
        {
            extent_ = box;
            mapnik::feature_kv_iterator f_itr = f->begin();
            mapnik::feature_kv_iterator f_end = f->end();
            for ( ;f_itr!=f_end; ++f_itr)
            {
                desc_.add_descriptor(mapnik::attribute_descriptor(std::get<0>(*f_itr),
                    boost::apply_visitor(attr_value_converter(),std::get<1>(*f_itr).base())));
            }
        }
        else
        {
            extent_.expand_to_include(box);
        }
        tree_.insert(box_type(point_type(box.minx(),box.miny()),point_type(box.maxx(),box.maxy())), count++);
    }
}

geojson_datasource::~geojson_datasource() { }

const char * geojson_datasource::name()
{
    return "geojson";
}

boost::optional<mapnik::datasource::geometry_t> geojson_datasource::get_geometry_type() const
{
    boost::optional<mapnik::datasource::geometry_t> result;
    int multi_type = 0;
    unsigned num_features = features_.size();
    for (unsigned i = 0; i < num_features && i < 5; ++i)
    {
        mapnik::util::to_ds_type(features_[i]->paths(),result);
        if (result)
        {
            int type = static_cast<int>(*result);
            if (multi_type > 0 && multi_type != type)
            {
                result.reset(mapnik::datasource::Collection);
                return result;
            }
            multi_type = type;
        }
    }
    return result;
}

mapnik::datasource::datasource_t geojson_datasource::type() const
{
    return type_;
}

mapnik::box2d<double> geojson_datasource::envelope() const
{
    return extent_;
}

mapnik::layer_descriptor geojson_datasource::get_descriptor() const
{
    return desc_;
}

mapnik::featureset_ptr geojson_datasource::features(mapnik::query const& q) const
{
    // if the query box intersects our world extent then query for features
    mapnik::box2d<double> const& b = q.get_bbox();
    if (extent_.intersects(b))
    {
        box_type box(point_type(b.minx(),b.miny()),point_type(b.maxx(),b.maxy()));
        return std::make_shared<geojson_featureset>(features_, tree_.find(box));
    }
    // otherwise return an empty featureset pointer
    return mapnik::featureset_ptr();
}

mapnik::featureset_ptr geojson_datasource::features_at_point(mapnik::coord2d const& pt, double tol) const
{
    mapnik::box2d<double> query_bbox(pt, pt);
    query_bbox.pad(tol);
    mapnik::query q(query_bbox);
    std::vector<mapnik::attribute_descriptor> const& desc = desc_.get_descriptors();
    std::vector<mapnik::attribute_descriptor>::const_iterator itr = desc.begin();
    std::vector<mapnik::attribute_descriptor>::const_iterator end = desc.end();
    for ( ;itr!=end;++itr)
    {
        q.add_property_name(itr->get_name());
    }
    return features(q);
}
