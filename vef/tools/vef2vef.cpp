/**
 * Copyright (c) 2021 Melown Technologies SE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * *  Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <cstdlib>
#include <string>
#include <iostream>

#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/optional/optional_io.hpp>

#include "dbglog/dbglog.hpp"

#include "service/cmdline.hpp"
#include "utility/gccversion.hpp"
#include "utility/buildsys.hpp"
#include "utility/implicit-value.hpp"

#include "math/transform.hpp"

#include "geometry/mesh.hpp"
#include "geometry/meshop.hpp"
#include "geometry/parse-obj.hpp"

#include "geo/csconvertor.hpp"
#include "geo/coordinates.hpp"
#include "geo/verticaladjuster.hpp"

#include "vef/vef.hpp"
#include "vef/reader.hpp"

namespace po = boost::program_options;
namespace fs = boost::filesystem;
namespace bio = boost::iostreams;

namespace {

class Vef2Vef : public service::Cmdline
{
public:
    Vef2Vef()
        : Cmdline("vef2vef", BUILD_TARGET_VERSION
                  , service::DISABLE_EXCESSIVE_LOGGING)
    {
    }

private:
    virtual void configuration(po::options_description &cmdline
                               , po::options_description &config
                               , po::positional_options_description &pd)
        UTILITY_OVERRIDE;

    virtual void configure(const po::variables_map &vars)
        UTILITY_OVERRIDE;

    virtual bool help(std::ostream &out, const std::string &what) const
        UTILITY_OVERRIDE;

    virtual int run() UTILITY_OVERRIDE;

    void convert(const vef::Archive &in, vef::ArchiveWriter &out);

    fs::path input_;
    fs::path output_;
    bool overwrite_ = false;

    boost::optional<geo::SrsDefinition> dstSrs_;
    bool verticalAdjustment_ = false;
};

void Vef2Vef::configuration(po::options_description &cmdline
                            , po::options_description &config
                            , po::positional_options_description &pd)
{
    cmdline.add_options()
        ("input",  po::value(&input_)->required()
         , "Path to input VEF dataset (directory, tar, zip).")

        ("output",  po::value(&output_)->required()
         , "Path to output VEF dataset (directory).")

        ("overwrite"
         , "Allows existing dataset overwrite if set.")

        ("dstSrs",  po::value<geo::SrsDefinition>()
         , "SRS of output dataset.")

        ("verticalAdjustment"
         ,  utility::implicit_value(&verticalAdjustment_, true)
         ->default_value(&verticalAdjustment_)
         , "Apply vertical adjustment to output data."
         "Dst SRS must be a projected system to apply.")
        ;

    pd
        .add("input", 1)
        .add("output", 1)
        ;

    (void) config;
}

void Vef2Vef::configure(const po::variables_map &vars)
{
    if (vars.count("dstSrs")) {
        dstSrs_ = vars["dstSrs"].as<geo::SrsDefinition>();
    }

    if (verticalAdjustment_) {
        if (!dstSrs_) {
            LOGTHROW(err2, std::runtime_error)
                << "Vertical adjustment makes no sense without "
                "destination SRS.";
        }

        if (!geo::isProjected(*dstSrs_)) {
            LOGTHROW(err2, std::runtime_error)
                << "Vertical adjustment makes sense only for "
                "projected SRS.";
        }
    }

    overwrite_ = vars.count("overwrite");
}

bool Vef2Vef::help(std::ostream &out, const std::string &what) const
{
    if (what.empty()) {
        out << R"RAW(vef2vef: applies transformation to existing VEF dataset
)RAW";
    }
    return false;
}

template <typename Point>
Point transform(const vef::OptionalMatrix &trafo
                , const Point &p)
{
    if (trafo) { return math::transform(*trafo, p); }
    return p;
}

class Convertor {
public:
    Convertor(const vef::OptionalMatrix &inTrafo = boost::none
              , const geo::CsConvertor &conv = geo::CsConvertor()
              , const geo::VerticalAdjuster &adjuster
                  = geo::VerticalAdjuster()
              , const vef::OptionalMatrix &outTrafo = boost::none)
        : inTrafo_(inTrafo)
        , conv_(conv), adjuster_(adjuster)
        , outTrafo_(outTrafo)
    {}

    template <typename Point>
    Point operator()(const Point &p) const {
        return transform(outTrafo_, adjuster_(conv_(transform(inTrafo_, p))));
    }

private:
    vef::OptionalMatrix inTrafo_;
    geo::CsConvertor conv_;
    geo::VerticalAdjuster adjuster_;
    vef::OptionalMatrix outTrafo_;
};

math::Extents2 measure(const vef::Archive &in
                       , const vef::LoddedWindow &window
                       , const geo::CsConvertor &conv)
{
    struct Measurer : geometry::ObjParserBase {
        const Convertor conv;
        math::Extents2 extents;

        Measurer(const Convertor &conv)
            : conv(conv)
            , extents(math::InvalidExtents{})
        {}

        void addTexture(const Vector3d&) override {}
        void addNormal(const Vector3d&) override {}
        void addFacet(const Facet&) override {}
        void materialLibrary(const std::string&) override {}
        void useMaterial(const std::string&) override {}

        void addVertex(const Vector3d &v) override {
            math::update(extents, conv(math::Point3(v.x, v.y, v.z)));
        }

    } measurer(Convertor(vef::windowMatrix(in.manifest(), window), conv));

    {
        const auto &mesh(window.lods.back().mesh);

        auto is(in.meshIStream(mesh));
        auto res(measurer.parse(is->get()));
        is->close();
        if (!res) {
            LOGTHROW(err2, std::runtime_error)
                << "Unable to measure mesh from " << mesh.path << ".";
        }
    }

    return measurer.extents;
}

math::Extents2 measure(const vef::Archive &in, const geo::CsConvertor &conv)
{
    math::Extents2 extents(math::InvalidExtents{});

    if (!conv) { return extents; }

    for (const auto &window : in.manifest().windows) {
        math::update(extents, measure(in, window, conv));
    }

    return extents;
}

geo::CsConvertor makeConv(const geo::SrsDefinition &in
                          , const boost::optional<geo::SrsDefinition> &out)
{
    if (out) { return { in, *out }; }
    return {};
}

struct DstTrafo {
    vef::OptionalMatrix fromGeo;
    vef::OptionalMatrix toGeo;

    DstTrafo(const vef::OptionalMatrix &fromGeo = boost::none
             , const vef::OptionalMatrix &toGeo = boost::none)
        : fromGeo(fromGeo), toGeo(toGeo)
    {}
};

DstTrafo makeDstTrafo(const math::Extents2 &extents)
{
    if (!math::valid(extents)) { return {}; }
    return { geo::geo2local(extents), geo::local2geo(extents) };
}

vef::OptionalMatrix makeInvDstTrafo(const math::Extents2 &extents)
{
    if (!math::valid(extents)) { return boost::none; }
    return geo::local2geo(extents);
}

void copyGzipped(std::istream &is, const fs::path &filepath)
{
    std::ofstream f;
    f.exceptions(std::ios::badbit | std::ios::failbit);
    try {
        fs::create_directories(fs::absolute(filepath).parent_path());
        f.open(filepath.string(), std::ios_base::out | std::ios_base::trunc);
    } catch (const std::exception&) {
        LOGTHROW(err3, std::runtime_error)
            << "Unable to save mesh to <" << filepath << ">.";
    }

    bio::filtering_ostream gzipped;
    gzipped.push(bio::gzip_compressor(bio::gzip_params(9), 1 << 16));
    gzipped.push(f);

    // copy data from source input stream to output
    gzipped << is.rdbuf();

    gzipped.flush();
}

void copy(std::istream &is, const fs::path &filepath)
{
    std::ofstream f;
    f.exceptions(std::ios::badbit | std::ios::failbit);
    try {
        fs::create_directories(fs::absolute(filepath).parent_path());
        f.open(filepath.string(), std::ios_base::out | std::ios_base::trunc);
    } catch (const std::exception&) {
        LOGTHROW(err3, std::runtime_error)
            << "Unable to save mesh to <" << filepath << ">.";
    }

    // copy data from source input stream to output
    f << is.rdbuf();
    f.flush();
}

void copyAtlas(const vef::Archive &in, vef::ArchiveWriter &out
               , const vef::Window &window
               , vef::Id oWindowId, vef::Id oLodId)
{
    for (const auto &texture : window.atlas) {
        auto oTexture(out.addTexture(oWindowId, oLodId
                                     , texture, texture.format));

        LOG(info3) << "Copying texture " << texture.path
                   << " to " << oTexture.path;
        {
            auto is(in.archive().istream(texture.path));
            copy(is->get(), oTexture.path);
            is->close();
        }
    }
}

void copyWindow(const vef::Archive &in, vef::ArchiveWriter &out
                , const vef::Window &window
                , vef::Id oWindowId, vef::Id oLodId)
{
    auto &oMesh(out.mesh(oWindowId, oLodId));

    LOG(info3) << "Copying mesh " << window.mesh.path
               << " to " << oMesh.path;
    {
        auto is(in.meshIStream(window.mesh));
        copyGzipped(is->get(), oMesh.path);
        is->close();
    }

    copyAtlas(in, out, window, oWindowId, oLodId);
}

void convertWindow(const vef::Archive &in, vef::ArchiveWriter &out
                   , const vef::Window &window
                   , vef::Id oWindowId, vef::Id oLodId
                   , const Convertor &conv)
{
    auto &oMesh(out.mesh(oWindowId, oLodId));

    LOG(info3) << "Converting mesh " << window.mesh.path
               << " to " << oMesh.path;

    struct ConvertingLoader : geometry::ObjParserBase {
        const Convertor conv;
        geometry::Mesh mesh;

        unsigned int imageId = 0;

        ConvertingLoader(const Convertor &conv)
            : conv(conv)
        {}

        void addNormal(const Vector3d&) override {}
        void materialLibrary(const std::string&) override {}

        void addFacet(const Facet &f) override {
            mesh.faces.emplace_back(f.v[0], f.v[1], f.v[2]
                                    , f.t[0], f.t[1], f.t[2]
                                    , imageId);
        }

        void useMaterial(const std::string &value) override {
            imageId = boost::lexical_cast<unsigned int>(value);
        }

        void addVertex(const Vector3d &v) override {
            mesh.vertices.push_back(conv(math::Point3(v.x, v.y, v.z)));
        }

        void addTexture(const Vector3d &t) override {
            mesh.tCoords.emplace_back(t.x, t.y);
        }

    } loader(conv);

    // load and convert mesh
    {
        auto is(in.meshIStream(window.mesh));
        auto res(loader.parse(is->get()));
        is->close();
        if (!res) {
            LOGTHROW(err2, std::runtime_error)
                << "Unable to load mesh from " << window.mesh.path << ".";
        }
    }

    // save converted mesh
    geometry::saveAsGzippedObj(loader.mesh, oMesh.path
                               , oMesh.mtlPath().filename().string());

    copyAtlas(in, out, window, oWindowId, oLodId);
}

void Vef2Vef::convert(const vef::Archive &in, vef::ArchiveWriter &out)
{
    const auto geoConv(makeConv(*in.manifest().srs, dstSrs_));
    const auto dstTrafo(makeDstTrafo(measure(in, geoConv)));

    const geo::VerticalAdjuster verticalAdjuster
        (dstSrs_
         ? geo::VerticalAdjuster()
         : geo::VerticalAdjuster(verticalAdjustment_, *dstSrs_));

    out.setTrafo(dstTrafo.toGeo);

    const auto &iManifest(in.manifest());
    for (const auto &iWindow : iManifest.windows) {
        Convertor conv(vef::windowMatrix(in.manifest(), iWindow)
                       , geoConv, verticalAdjuster, dstTrafo.fromGeo);

        const auto oWindowId(out.addWindow());
        for (const auto &iLod : iWindow.lods) {
            const auto oLodId(out.addLod(oWindowId, boost::none
                                         , vef::Mesh::Format::gzippedObj));

            if (dstSrs_) {
                convertWindow(in, out, iLod, oWindowId, oLodId, conv);
            } else {
                copyWindow(in, out, iLod, oWindowId, oLodId);
            }
        }
    }
}

int Vef2Vef::run()
{
    vef::Archive in(input_);

    if (dstSrs_ && !in.manifest().srs) {
        std::cerr << "Input VEF dataset has no SRS, cannot transform."
                  << std::endl;
        return EXIT_FAILURE;
    }

    vef::ArchiveWriter out(output_, overwrite_);

    if (dstSrs_) { out.setSrs(*dstSrs_); }

    convert(in, out);

    out.flush();

    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char *argv[])
{
    return Vef2Vef()(argc, argv);
}
