/*
 * OpenSceneGraph ASTC (Adaptive Scalable Texture Compression) plugin
 * 
 * This plugin provides support for reading and writing ASTC compressed texture files
 * using the libastcenc library from ARM.
 */

#include <osg/Image>
#include <osg/Notify>
#include <osg/Geode>
#include <osg/GL>

#include <osgDB/Registry>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/fstream>

#include <astcenc.h>

#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>

// ASTC file header structure (16 bytes)
struct astc_header {
    uint8_t magic[4];      // 0x13, 0xAB, 0xA1, 0x5C
    uint8_t block_x;       // Block width in texels
    uint8_t block_y;       // Block height in texels  
    uint8_t block_z;       // Block depth in texels (1 for 2D)
    uint8_t dim_x[3];      // Image width in texels (little-endian)
    uint8_t dim_y[3];      // Image height in texels (little-endian)
    uint8_t dim_z[3];      // Image depth in texels (little-endian, 1 for 2D)
};

// Helper function to decode 3-byte little-endian value
static uint32_t decode_uint24(const uint8_t data[3]) {
    return data[0] | (data[1] << 8) | (data[2] << 16);
}

// Helper function to encode 3-byte little-endian value
static void encode_uint24(uint8_t data[3], uint32_t value) {
    data[0] = value & 0xFF;
    data[1] = (value >> 8) & 0xFF;
    data[2] = (value >> 16) & 0xFF;
}

// Map ASTC block size to OpenGL format
static unsigned int getGLFormat(uint8_t block_x, uint8_t block_y, bool is_srgb = false) {
    if (is_srgb) {
        switch ((block_x << 8) | block_y) {
            case (4 << 8) | 4:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR;
            case (5 << 8) | 4:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR;
            case (5 << 8) | 5:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR;
            case (6 << 8) | 5:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR;
            case (6 << 8) | 6:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR;
            case (8 << 8) | 5:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR;
            case (8 << 8) | 6:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR;
            case (8 << 8) | 8:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR;
            case (10 << 8) | 5:  return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x5_KHR;
            case (10 << 8) | 6:  return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x6_KHR;
            case (10 << 8) | 8:  return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x8_KHR;
            case (10 << 8) | 10: return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10_KHR;
            case (12 << 8) | 10: return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x10_KHR;
            case (12 << 8) | 12: return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x12_KHR;
        }
    } else {
        switch ((block_x << 8) | block_y) {
            case (4 << 8) | 4:   return GL_COMPRESSED_RGBA_ASTC_4x4_KHR;
            case (5 << 8) | 4:   return GL_COMPRESSED_RGBA_ASTC_5x4_KHR;
            case (5 << 8) | 5:   return GL_COMPRESSED_RGBA_ASTC_5x5_KHR;
            case (6 << 8) | 5:   return GL_COMPRESSED_RGBA_ASTC_6x5_KHR;
            case (6 << 8) | 6:   return GL_COMPRESSED_RGBA_ASTC_6x6_KHR;
            case (8 << 8) | 5:   return GL_COMPRESSED_RGBA_ASTC_8x5_KHR;
            case (8 << 8) | 6:   return GL_COMPRESSED_RGBA_ASTC_8x6_KHR;
            case (8 << 8) | 8:   return GL_COMPRESSED_RGBA_ASTC_8x8_KHR;
            case (10 << 8) | 5:  return GL_COMPRESSED_RGBA_ASTC_10x5_KHR;
            case (10 << 8) | 6:  return GL_COMPRESSED_RGBA_ASTC_10x6_KHR;
            case (10 << 8) | 8:  return GL_COMPRESSED_RGBA_ASTC_10x8_KHR;
            case (10 << 8) | 10: return GL_COMPRESSED_RGBA_ASTC_10x10_KHR;
            case (12 << 8) | 10: return GL_COMPRESSED_RGBA_ASTC_12x10_KHR;
            case (12 << 8) | 12: return GL_COMPRESSED_RGBA_ASTC_12x12_KHR;
        }
    }
    return 0; // Unsupported block size
}

// Get block size from GL format
static bool getBlockSize(unsigned int format, uint8_t& block_x, uint8_t& block_y) {
    switch (format) {
        case GL_COMPRESSED_RGBA_ASTC_4x4_KHR:
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR:
            block_x = 4; block_y = 4; return true;
        case GL_COMPRESSED_RGBA_ASTC_5x4_KHR:
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR:
            block_x = 5; block_y = 4; return true;
        case GL_COMPRESSED_RGBA_ASTC_5x5_KHR:
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR:
            block_x = 5; block_y = 5; return true;
        case GL_COMPRESSED_RGBA_ASTC_6x5_KHR:
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR:
            block_x = 6; block_y = 5; return true;
        case GL_COMPRESSED_RGBA_ASTC_6x6_KHR:
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR:
            block_x = 6; block_y = 6; return true;
        case GL_COMPRESSED_RGBA_ASTC_8x5_KHR:
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR:
            block_x = 8; block_y = 5; return true;
        case GL_COMPRESSED_RGBA_ASTC_8x6_KHR:
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR:
            block_x = 8; block_y = 6; return true;
        case GL_COMPRESSED_RGBA_ASTC_8x8_KHR:
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR:
            block_x = 8; block_y = 8; return true;
        case GL_COMPRESSED_RGBA_ASTC_10x5_KHR:
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x5_KHR:
            block_x = 10; block_y = 5; return true;
        case GL_COMPRESSED_RGBA_ASTC_10x6_KHR:
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x6_KHR:
            block_x = 10; block_y = 6; return true;
        case GL_COMPRESSED_RGBA_ASTC_10x8_KHR:
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x8_KHR:
            block_x = 10; block_y = 8; return true;
        case GL_COMPRESSED_RGBA_ASTC_10x10_KHR:
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10_KHR:
            block_x = 10; block_y = 10; return true;
        case GL_COMPRESSED_RGBA_ASTC_12x10_KHR:
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x10_KHR:
            block_x = 12; block_y = 10; return true;
        case GL_COMPRESSED_RGBA_ASTC_12x12_KHR:
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x12_KHR:
            block_x = 12; block_y = 12; return true;
        default:
            return false;
    }
}

class ReaderWriterASTC : public osgDB::ReaderWriter
{
public:
    ReaderWriterASTC()
    {
        supportsExtension("astc", "ASTC compressed texture format");
        supportsOption("astc_linear", "Treat texture as linear (default is sRGB when appropriate)");
        supportsOption("astc_srgb", "Force sRGB interpretation");
        
        // Block size options for compression
        supportsOption("astc_4x4", "Use 4x4 block size for compression (default, 8.00 bpp)");
        supportsOption("astc_5x4", "Use 5x4 block size for compression (6.40 bpp)");
        supportsOption("astc_5x5", "Use 5x5 block size for compression (5.12 bpp)");
        supportsOption("astc_6x5", "Use 6x5 block size for compression (4.27 bpp)");
        supportsOption("astc_6x6", "Use 6x6 block size for compression (3.56 bpp)");
        supportsOption("astc_8x5", "Use 8x5 block size for compression (3.20 bpp)");
        supportsOption("astc_8x6", "Use 8x6 block size for compression (2.67 bpp)");
        supportsOption("astc_8x8", "Use 8x8 block size for compression (2.00 bpp)");
        supportsOption("astc_10x5", "Use 10x5 block size for compression (2.56 bpp)");
        supportsOption("astc_10x6", "Use 10x6 block size for compression (2.13 bpp)");
        supportsOption("astc_10x8", "Use 10x8 block size for compression (1.60 bpp)");
        supportsOption("astc_10x10", "Use 10x10 block size for compression (1.28 bpp)");
        supportsOption("astc_12x10", "Use 12x10 block size for compression (1.07 bpp)");
        supportsOption("astc_12x12", "Use 12x12 block size for compression (0.89 bpp)");
        
        // Quality/speed options
        supportsOption("astc_fast", "Use fast compression preset");
        supportsOption("astc_medium", "Use medium compression preset (default)");
        supportsOption("astc_thorough", "Use thorough compression preset");
        supportsOption("astc_exhaustive", "Use exhaustive compression preset");
        
        // Profile options
        supportsOption("astc_hdr", "Use HDR profile for high dynamic range images");
    }

    virtual const char* className() const override
    {
        return "ASTC Image Reader/Writer";
    }

    virtual ReadResult readObject(const std::string& file, const osgDB::ReaderWriter::Options* options) const override
    {
        return readImage(file, options);
    }

    virtual ReadResult readObject(std::istream& fin, const Options* options) const override
    {
        return readImage(fin, options);
    }

    virtual ReadResult readImage(const std::string& file, const osgDB::ReaderWriter::Options* options) const override
    {
        std::string ext = osgDB::getLowerCaseFileExtension(file);
        if (!acceptsExtension(ext)) return ReadResult::FILE_NOT_HANDLED;

        std::string fileName = osgDB::findDataFile(file, options);
        if (fileName.empty()) return ReadResult::FILE_NOT_FOUND;

        osgDB::ifstream stream(fileName.c_str(), std::ios::in | std::ios::binary);
        if (!stream) return ReadResult::FILE_NOT_HANDLED;
        
        ReadResult rr = readImage(stream, options);
        if (rr.validImage()) rr.getImage()->setFileName(file);
        return rr;
    }

    virtual ReadResult readImage(std::istream& fin, const Options* options) const override
    {
        // Parse options
        bool force_linear = false;
        bool force_srgb = false;
        
        if (options) {
            std::istringstream iss(options->getOptionString());
            std::string opt;
            while (iss >> opt) {
                if (opt == "astc_linear") force_linear = true;
                if (opt == "astc_srgb") force_srgb = true;
            }
        }

        // Read ASTC header
        astc_header header;
        fin.read(reinterpret_cast<char*>(&header), sizeof(header));
        
        if (fin.gcount() != sizeof(header)) {
            OSG_WARN << "ReadASTC: Could not read ASTC header" << std::endl;
            return ReadResult::FILE_NOT_HANDLED;
        }

        // Verify magic number
        if (header.magic[0] != 0x13 || header.magic[1] != 0xAB || 
            header.magic[2] != 0xA1 || header.magic[3] != 0x5C) {
            OSG_WARN << "ReadASTC: Invalid ASTC magic number" << std::endl;
            return ReadResult::FILE_NOT_HANDLED;
        }

        // Decode dimensions
        uint32_t width = decode_uint24(header.dim_x);
        uint32_t height = decode_uint24(header.dim_y);
        uint32_t depth = decode_uint24(header.dim_z);

        if (depth != 1) {
            OSG_WARN << "ReadASTC: 3D ASTC textures not supported" << std::endl;
            return ReadResult::FILE_NOT_HANDLED;
        }

        OSG_INFO << "ReadASTC: " << width << "x" << height << " block size " 
                 << (int)header.block_x << "x" << (int)header.block_y << std::endl;

        // Calculate compressed data size
        uint32_t blocks_x = (width + header.block_x - 1) / header.block_x;
        uint32_t blocks_y = (height + header.block_y - 1) / header.block_y;
        uint32_t data_size = blocks_x * blocks_y * 16; // Each ASTC block is 16 bytes

        // Read compressed data
        std::vector<uint8_t> compressed_data(data_size);
        fin.read(reinterpret_cast<char*>(compressed_data.data()), data_size);
        
        if (fin.gcount() != data_size) {
            OSG_WARN << "ReadASTC: Could not read compressed data" << std::endl;
            return ReadResult::FILE_NOT_HANDLED;
        }

        // Determine OpenGL format
        bool use_srgb = force_srgb && !force_linear;
        unsigned int gl_format = getGLFormat(header.block_x, header.block_y, use_srgb);
        
        if (gl_format == 0) {
            OSG_WARN << "ReadASTC: Unsupported block size " 
                     << (int)header.block_x << "x" << (int)header.block_y << std::endl;
            return ReadResult::FILE_NOT_HANDLED;
        }

        // Create OSG Image
        osg::ref_ptr<osg::Image> image = new osg::Image();
        
        // Allocate and copy data
        uint8_t* image_data = new uint8_t[data_size];
        memcpy(image_data, compressed_data.data(), data_size);
        
        image->setImage(width, height, 1, gl_format, gl_format, GL_UNSIGNED_BYTE, 
                       image_data, osg::Image::USE_NEW_DELETE, 16);

        return image.release();
    }

    virtual WriteResult writeObject(const osg::Object& object, const std::string& file, 
                                   const osgDB::ReaderWriter::Options* options) const override
    {
        const osg::Image* image = dynamic_cast<const osg::Image*>(&object);
        if (!image) return WriteResult::FILE_NOT_HANDLED;

        return writeImage(*image, file, options);
    }

    virtual WriteResult writeObject(const osg::Object& object, std::ostream& fout, 
                                   const Options* options) const override
    {
        const osg::Image* image = dynamic_cast<const osg::Image*>(&object);
        if (!image) return WriteResult::FILE_NOT_HANDLED;

        return writeImage(*image, fout, options);
    }

    virtual WriteResult writeImage(const osg::Image& image, const std::string& file, 
                                  const osgDB::ReaderWriter::Options* options) const override
    {
        std::string ext = osgDB::getFileExtension(file);
        if (!acceptsExtension(ext)) return WriteResult::FILE_NOT_HANDLED;

        osgDB::ofstream fout(file.c_str(), std::ios::out | std::ios::binary);
        if (!fout) return WriteResult::ERROR_IN_WRITING_FILE;

        return writeImage(image, fout, options);
    }

    virtual WriteResult writeImage(const osg::Image& image, std::ostream& fout, 
                                  const Options* options) const override
    {
        // Check if image is already ASTC compressed
        uint8_t block_x, block_y;
        if (getBlockSize(image.getPixelFormat(), block_x, block_y)) {
            // Image is already ASTC compressed, write directly
            return writeCompressedASTC(image, fout, block_x, block_y);
        } else {
            // Image needs compression - use libastcenc
            return compressAndWriteASTC(image, fout, options);
        }
    }

private:
    WriteResult writeCompressedASTC(const osg::Image& image, std::ostream& fout, 
                                   uint8_t block_x, uint8_t block_y) const
    {
        // Create ASTC header
        astc_header header;
        header.magic[0] = 0x13;
        header.magic[1] = 0xAB;
        header.magic[2] = 0xA1;
        header.magic[3] = 0x5C;
        header.block_x = block_x;
        header.block_y = block_y;
        header.block_z = 1;
        
        encode_uint24(header.dim_x, image.s());
        encode_uint24(header.dim_y, image.t());
        encode_uint24(header.dim_z, 1);

        // Write header
        fout.write(reinterpret_cast<const char*>(&header), sizeof(header));
        
        // Write compressed data
        fout.write(reinterpret_cast<const char*>(image.data()), image.getTotalSizeInBytes());

        if (!fout.good()) return WriteResult::ERROR_IN_WRITING_FILE;
        return WriteResult::FILE_SAVED;
    }

    WriteResult compressAndWriteASTC(const osg::Image& image, std::ostream& fout, 
                                    const Options* options) const
    {
        // Parse compression options
        uint8_t block_x = 4, block_y = 4; // Default block size
        astcenc_profile profile = ASTCENC_PRF_LDR;
        float quality = ASTCENC_PRE_MEDIUM;
        
        if (options) {
            std::istringstream iss(options->getOptionString());
            std::string opt;
            while (iss >> opt) {
                if (opt == "astc_4x4") { block_x = 4; block_y = 4; }
                else if (opt == "astc_5x4") { block_x = 5; block_y = 4; }
                else if (opt == "astc_5x5") { block_x = 5; block_y = 5; }
                else if (opt == "astc_6x5") { block_x = 6; block_y = 5; }
                else if (opt == "astc_6x6") { block_x = 6; block_y = 6; }
                else if (opt == "astc_8x5") { block_x = 8; block_y = 5; }
                else if (opt == "astc_8x6") { block_x = 8; block_y = 6; }
                else if (opt == "astc_8x8") { block_x = 8; block_y = 8; }
                else if (opt == "astc_10x5") { block_x = 10; block_y = 5; }
                else if (opt == "astc_10x6") { block_x = 10; block_y = 6; }
                else if (opt == "astc_10x8") { block_x = 10; block_y = 8; }
                else if (opt == "astc_10x10") { block_x = 10; block_y = 10; }
                else if (opt == "astc_12x10") { block_x = 12; block_y = 10; }
                else if (opt == "astc_12x12") { block_x = 12; block_y = 12; }
                else if (opt == "astc_fast") quality = ASTCENC_PRE_FAST;
                else if (opt == "astc_medium") quality = ASTCENC_PRE_MEDIUM;
                else if (opt == "astc_thorough") quality = ASTCENC_PRE_THOROUGH;
                else if (opt == "astc_exhaustive") quality = ASTCENC_PRE_EXHAUSTIVE;
                else if (opt == "astc_hdr") profile = ASTCENC_PRF_HDR;
            }
        }

        // Convert OSG image to astcenc format
        astcenc_image astc_image;
        astc_image.dim_x = image.s();
        astc_image.dim_y = image.t();
        astc_image.dim_z = 1;
        
        // Determine data format and convert if necessary
        std::vector<uint8_t> converted_data;
        const uint8_t* source_data = image.data();
        
        switch (image.getPixelFormat()) {
            case GL_RGBA:
                astc_image.data_type = ASTCENC_TYPE_U8;
                astc_image.data = const_cast<void*>(reinterpret_cast<const void*>(source_data));
                break;
            case GL_RGB:
                {
                    // Convert RGB to RGBA
                    astc_image.data_type = ASTCENC_TYPE_U8;
                    size_t pixel_count = image.s() * image.t();
                    converted_data.resize(pixel_count * 4);
                    
                    for (size_t i = 0; i < pixel_count; ++i) {
                        converted_data[i * 4 + 0] = source_data[i * 3 + 0]; // R
                        converted_data[i * 4 + 1] = source_data[i * 3 + 1]; // G
                        converted_data[i * 4 + 2] = source_data[i * 3 + 2]; // B
                        converted_data[i * 4 + 3] = 255; // A
                    }
                    astc_image.data = converted_data.data();
                }
                break;
            default:
                OSG_WARN << "WriteASTC: Unsupported pixel format for compression" << std::endl;
                return WriteResult::FILE_NOT_HANDLED;
        }

        // Initialize astcenc configuration
        astcenc_config config;
        astcenc_error error = astcenc_config_init(profile, block_x, block_y, 1, quality, 0, &config);
        if (error != ASTCENC_SUCCESS) {
            OSG_WARN << "WriteASTC: Failed to initialize astcenc config: " << astcenc_get_error_string(error) << std::endl;
            return WriteResult::ERROR_IN_WRITING_FILE;
        }

        // Create astcenc context
        astcenc_context* context;
        error = astcenc_context_alloc(&config, 1, &context);
        if (error != ASTCENC_SUCCESS) {
            OSG_WARN << "WriteASTC: Failed to create astcenc context: " << astcenc_get_error_string(error) << std::endl;
            return WriteResult::ERROR_IN_WRITING_FILE;
        }

        // Calculate compressed size
        uint32_t blocks_x = (astc_image.dim_x + block_x - 1) / block_x;
        uint32_t blocks_y = (astc_image.dim_y + block_y - 1) / block_y;
        size_t compressed_size = blocks_x * blocks_y * 16;

        // Allocate compression buffer
        std::vector<uint8_t> compressed_data(compressed_size);

        // Perform compression
        error = astcenc_compress_image(context, &astc_image, nullptr, compressed_data.data(), compressed_size, 0);
        if (error != ASTCENC_SUCCESS) {
            OSG_WARN << "WriteASTC: Compression failed: " << astcenc_get_error_string(error) << std::endl;
            astcenc_context_free(context);
            return WriteResult::ERROR_IN_WRITING_FILE;
        }

        astcenc_context_free(context);

        // Create ASTC header
        astc_header header;
        header.magic[0] = 0x13;
        header.magic[1] = 0xAB;
        header.magic[2] = 0xA1;
        header.magic[3] = 0x5C;
        header.block_x = block_x;
        header.block_y = block_y;
        header.block_z = 1;
        
        encode_uint24(header.dim_x, astc_image.dim_x);
        encode_uint24(header.dim_y, astc_image.dim_y);
        encode_uint24(header.dim_z, 1);

        // Write header and data
        fout.write(reinterpret_cast<const char*>(&header), sizeof(header));
        fout.write(reinterpret_cast<const char*>(compressed_data.data()), compressed_size);

        if (!fout.good()) return WriteResult::ERROR_IN_WRITING_FILE;
        
        OSG_INFO << "WriteASTC: Compressed " << astc_image.dim_x << "x" << astc_image.dim_y 
                 << " to ASTC " << (int)block_x << "x" << (int)block_y << std::endl;
        
        return WriteResult::FILE_SAVED;
    }
};

// Register with Registry
REGISTER_OSGPLUGIN(astc, ReaderWriterASTC)