// -*- C++ -*-  Copyright (c) Microsoft Corporation; see license.txt
#ifndef MESH_PROCESSING_LIBHH_IMAGE_H_
#define MESH_PROCESSING_LIBHH_IMAGE_H_

#include "libHh/Array.h"
#include "libHh/Matrix.h"
#include "libHh/MatrixOp.h"
#include "libHh/ParallelCoords.h"
#include "libHh/RangeOp.h"  // fill()
#include "libHh/Vector4.h"
#include "libHh/Vector4i.h"

#if 0
{
  Image image(V(ysize, xsize)), image2(image.dims(), Pixel(65, 66, 67, 200));
  image2.set_zsize(4);
  Pixel& pixel = image[y][x];
  uint8_t c = image(y, x)[z];
  // ysize() == #rows, xsize() == #columns, zsize() == #channels.
  // Valid values for zsize(): 1: grayscale image: Red == Green == Blue  (Alpha is undefined);
  //                           3: RGB image  (default)  (Alpha is undefined);
  //                           4: RGBA image.
  // Row-column origin is at upper-left corner!
  //  (This is consistent with *.jpg, *.png, *.ppm, DirectX.)
  //  (In contrast, *.rgb, *.bmp, OpenGL glTexImage2D() have image origin at lower-left.)
  // bgra == true -> BGR or BGRA channel-ordering.
  // Image IO is performed using: Windows Imaging Component (wic), libpng + libjpeg + .. (libs), or ffmpeg (ffmpeg).
}
#endif

namespace hh {

namespace details {
struct ImageLibs;
}

class Image : public Matrix<Pixel> {
  using base = Matrix<Pixel>;
  friend void swap(Image& l, Image& r) noexcept;

 public:
  struct Attrib;
  explicit Image(const Vec2<int>& pdims = V(0, 0));
  explicit Image(const Vec2<int>& pdims, Pixel pixel) : Image(pdims) { fill(*this, pixel); }
  explicit Image(const Image&) = default;
  explicit Image(const base& image) : base(image.dims()) { base::assign(image); }
  explicit Image(const string& filename) { read_file(filename); }
  Image(Image&& m) noexcept { swap(*this, m); }
  Image(base&& m) noexcept { swap(implicit_cast<base&>(*this), m); }
  Image& operator=(Image&& image) noexcept;
  void operator=(base&& image) { clear(), swap(implicit_cast<base&>(*this), image); }
  Image& operator=(const Image&) = default;
  void operator=(CMatrixView<Pixel> image) { base::assign(image); }
  void init(const Vec2<int>& pdims) { base::init(pdims); }
  void init(const Vec2<int>& pdims, Pixel pixel);
  void clear() { init(twice(0)); }
  const Attrib& attrib() const { return _attrib; }
  Attrib& attrib() { return _attrib; }
  void set_zsize(int n);
  int zsize() const { return attrib().zsize; }
  void set_suffix(string suffix) { attrib().suffix = std::move(suffix); }  // e.g. "jpg"; for writing '-'.
  const string& suffix() const { return attrib().suffix; }                 // e.g. "rgb"; used for reading '-'.
  void set_silent_io_progress(bool b) { _silent_io_progress = b; }
  // Filename may be "-" for std::cin;  may throw std::runtime_error.
  void read_file(const string& filename) { read_file_i(filename, false); }
  void read_file_bgra(const string& filename) { read_file_i(filename, true); }
  // Filename may be "-" for std::cout; may throw std::runtime_error; suffix() is mutable.
  void write_file(const string& filename) const { write_file_i(filename, false); }
  void write_file_bgra(const string& filename) const { write_file_i(filename, true); }

  // Misc:
  void to_bw();
  void to_color();
  void scale(const Vec2<float>& syx, const Vec2<FilterBnd>& filterbs, const Pixel* bordervalue = nullptr);
  struct Attrib {
    int zsize{3};            // Number of components per pixel; must be either 1, 3, or 4.
    string suffix;           // e.g. "rgb"; "" if unknown; to identify format of read_file("-") and write_file("-").
   private:                  // Reserved for Image class internals.
    Array<uchar> exif_data;  // Zero length if absent; could also be implemented easily as string.
    string orig_filename;    // For Image_wic, upon writing image, it can reopen source file to copy metadata.
    string orig_suffix;      // For Image_wic, indicates type of original image container.
    friend Image;
    friend details::ImageLibs;
  };

 private:
  Attrib _attrib;
  bool _silent_io_progress{false};
  void read_file_i(const string& filename, bool bgra);
  void write_file_i(const string& filename, bool bgra) const;
  friend details::ImageLibs;
  void read_file_wic(const string& filename, bool bgra);
  void write_file_wic(const string& filename, bool bgra) const;
  void read_file_libs(const string& filename, bool bgra);
  void write_file_libs(const string& filename, bool bgra) const;
  void read_file_ffmpeg(const string& filename, bool bgra);
  void write_file_ffmpeg(const string& filename, bool bgra) const;
};

// Whether filename suffix identifies it as an image.
bool filename_is_image(const string& filename);

// Return predicted image suffix given first byte of file, or "" if unrecognized.
string image_suffix_for_magic_byte(uchar c);

// &image == &newimage is OK.
Image scale(const Image& image, const Vec2<float>& syx, const Vec2<FilterBnd>& filterbs,
            const Pixel* bordervalue = nullptr, Image&& newimage = Image());

inline bool rgb_equal(const Pixel& pix1, const Pixel& pix2) {
  return pix1[0] == pix2[0] && pix1[1] == pix2[1] && pix1[2] == pix2[2];
}

inline bool equal(const Pixel& pix1, const Pixel& pix2, int nz) {
  ASSERTX(nz >= 3);
  return rgb_equal(pix1, pix2) && (nz < 4 || pix1[3] == pix2[3]);
}

inline int rgb_dist2(const Pixel& pix1, const Pixel& pix2) {
  return square(int{pix1[0]} - pix2[0]) + square(int{pix1[1]} - pix2[1]) + square(int{pix1[2]} - pix2[2]);
}

// Convert float/double matrix (with 0.f == black and 1.f == white) to an image.
template <typename T> Image as_image(CMatrixView<T> matrix) {
  static_assert(std::is_floating_point_v<T>, "T must be float/double");
  // (Renamed image to image1 due to Visual Studio bug "C4459: declaration ... hides global declaration".)
  Image image1(matrix.dims());
  parallel_for(range(image1.ysize()), [&](const int y) {
    for_int(x, image1.xsize()) {
      image1[y][x] = Pixel::gray(narrow_cast<uint8_t>(clamp(matrix[y][x], T{0}, T{1}) * 255.f + .5f));
    }
  });
  return image1;
}

// Specialize as_image() to grid of Vector4.
inline Image as_image(CMatrixView<Vector4> grid) {
  Image image(grid.dims());
  parallel_for_coords({.cycles_per_elem = 10}, image.dims(), [&](const Vec2<int>& yx) {
    image[yx] = grid[yx].pixel();
    if (0) image[yx][3] = 255;
    if (image[yx][3] != 255) image.set_zsize(4);
  });
  return image;
}

// Specialize as_image() to grid of Vec3<float>.
inline Image as_image(CMatrixView<Vec3<float>> grid) {
  Image image(grid.dims());
  parallel_for_coords({.cycles_per_elem = 10}, image.dims(), [&](const Vec2<int>& yx) {  //
    image[yx] = Vector4(concat(grid[yx], V(1.f))).pixel();
  });
  return image;
}

// Specialize as_image() to grid of pixels.
inline Image as_image(CMatrixView<Pixel> grid) {
  Image image(grid.dims());
  parallel_for_coords(image.dims(), [&](const Vec2<int>& yx) { image[yx] = grid[yx]; });
  return image;
}

// *** Conversions between YUV and RGB color spaces.

// Image consisting of an 8-bit luminance matrix and a 2*8-bit chroma matrix at half spatial resolution.
class Nv12 {
 public:
  Nv12() = default;
  explicit Nv12(const Vec2<int>& dims) { init(dims); }
  void init(const Vec2<int>& dims) {
    _mat_Y.init(dims);
    _mat_UV.init(dims / 2);
    assertx(_mat_Y.dims() == _mat_UV.dims() * 2);
  }
  MatrixView<uint8_t> get_Y() { return _mat_Y; }
  CMatrixView<uint8_t> get_Y() const { return _mat_Y; }
  MatrixView<Vec2<uint8_t>> get_UV() { return _mat_UV; }
  CMatrixView<Vec2<uint8_t>> get_UV() const { return _mat_UV; }

 private:
  Matrix<uint8_t> _mat_Y;         // Luminance.
  Matrix<Vec2<uint8_t>> _mat_UV;  // Chroma at half the spatial resolution.
};

// View of an image consisting of an 8-bit luminance matrix and 2*8-bit chroma at half spatial resolution.
class Nv12View {
 public:
  Nv12View(MatrixView<uint8_t> mat_Y, MatrixView<Vec2<uint8_t>> mat_UV) : _mat_Y(mat_Y), _mat_UV(mat_UV) {
    assertx(_mat_Y.dims() == _mat_UV.dims() * 2);
  }
  Nv12View(Nv12& nv12) : _mat_Y(nv12.get_Y()), _mat_UV(nv12.get_UV()) {}
  MatrixView<uint8_t> get_Y() { return _mat_Y; }
  CMatrixView<uint8_t> get_Y() const { return _mat_Y; }
  MatrixView<Vec2<uint8_t>> get_UV() { return _mat_UV; }
  CMatrixView<Vec2<uint8_t>> get_UV() const { return _mat_UV; }

 private:
  MatrixView<uint8_t> _mat_Y;         // Luminance.
  MatrixView<Vec2<uint8_t>> _mat_UV;  // Chroma at half the spatial resolution.
};

// Constant view of an image consisting of an 8-bit luminance matrix and 2*8-bit chroma at half spatial resolution.
class CNv12View {
 public:
  CNv12View(CMatrixView<uint8_t> mat_Y, CMatrixView<Vec2<uint8_t>> mat_UV) : _mat_Y(mat_Y), _mat_UV(mat_UV) {
    assertx(_mat_Y.dims() == _mat_UV.dims() * 2);
  }
  CNv12View(const Nv12& nv12) : _mat_Y(nv12.get_Y()), _mat_UV(nv12.get_UV()) {}
  CNv12View(const Nv12View& nv12v) : _mat_Y(nv12v.get_Y()), _mat_UV(nv12v.get_UV()) {}
  void reinit(CNv12View nv12v) { _mat_Y.reinit(nv12v.get_Y()), _mat_UV.reinit(nv12v.get_UV()); }
  CMatrixView<uint8_t> get_Y() const { return _mat_Y; }
  CMatrixView<Vec2<uint8_t>> get_UV() const { return _mat_UV; }

 private:
  CMatrixView<uint8_t> _mat_Y;         // Luminance.
  CMatrixView<Vec2<uint8_t>> _mat_UV;  // Chroma at half the spatial resolution.
};

// Listed as "numerical approximations" on https://en.wikipedia.org/wiki/YUV
// Also found at https://learn.microsoft.com/en-us/previous-versions/aa917087(v=msdn.10)
// This may not correspond to YUV format used in ATSC HD video?
// Also, it condenses Y to range [16, 235]; strange.  Thus Y_from_RGB(Pixel::gray(128)) == 126.

// Convert RGB Pixel to luminance Y value.
inline uint8_t Y_from_RGB(const Pixel& pixel) {  // LumaFromRGB_CCIR601YCbCr.
  return narrow_cast<uint8_t>((66 * int{pixel[0]} + 129 * int{pixel[1]} + 25 * int{pixel[2]} + 128 + 16 * 256) >> 8);
}

// Convert RGB Pixel to chroma U value.
inline uint8_t U_from_RGB(const Pixel& pixel) {  // CbFromRGB_CCIR601YCbCr.
  return narrow_cast<uint8_t>((-38 * pixel[0] - 74 * pixel[1] + 112 * pixel[2] + 128 + 128 * 256) >> 8);
}

// Convert RGB Pixel to chroma V value.
inline uint8_t V_from_RGB(const Pixel& pixel) {  // CrFromRGB_CCIR601YCbCr.
  return narrow_cast<uint8_t>((112 * pixel[0] - 94 * pixel[1] - 18 * pixel[2] + 128 + 128 * 256) >> 8);
}

// Convert {R, G, B} values to YUV Vector4i.
inline Vector4i YUV_Vector4i_from_RGB(int r, int g, int b) {
  return ((r * Vector4i(66, -38, 112, 0) + g * Vector4i(129, -74, -94, 0) + b * Vector4i(25, 112, -18, 0) +
           Vector4i(128 + 16 * 256, 128 + 128 * 256, 128 + 128 * 256, 255 * 256)) >>
          8);
}

// Convert {R, G, B} values to YUV Pixel.
inline Pixel YUV_Pixel_from_RGB(int r, int g, int b) { return YUV_Vector4i_from_RGB(r, g, b).pixel(); }

// Convert {Y, U, V} values to RGB Vector4i.
inline Vector4i RGB_Vector4i_from_YUV(int y, int u, int v) {
  return ((y * Vector4i(298, 298, 298, 0) + u * Vector4i(0, -100, 516, 0) + v * Vector4i(409, -208, 0, 0) +
           Vector4i(128 - 298 * 16 - 409 * 128, 128 - 298 * 16 + 100 * 128 + 208 * 128, 128 - 298 * 16 - 516 * 128,
                    255 * 256)) >>
          8);
}

// Convert {Y, U, V} values to RGB Pixel.
inline Pixel RGB_Pixel_from_YUV(int y, int u, int v) { return RGB_Vector4i_from_YUV(y, u, v).pixel(); }

// Convert 8-bit luminance plus half-spatial resolution UV chroma to RGBA image.
void convert_Nv12_to_Image(CNv12View nv12v, MatrixView<Pixel> frame);

// Convert 8-bit luminance plus half-spatial resolution UV chroma to BGRA image.
void convert_Nv12_to_Image_BGRA(CNv12View nv12v, MatrixView<Pixel> frame);

// Convert RGBA image to 8-bit luminance plus half-spatial resolution UV chroma.
void convert_Image_to_Nv12(CMatrixView<Pixel> frame, Nv12View nv12v);

void scale(CNv12View nv12, const Vec2<FilterBnd>& filterbs, const Pixel* bordervalue, Nv12View new_nv12);

// *** Image IO.

#if 0  // Omitting WIC may allow writing image domains larger than 4 GiB.
#define HH_NO_IMAGE_WIC
#endif

#if 0  // Temporarily enable Image_libs even when WIC is available.
#define HH_IMAGE_LIBS_TOO
#endif

#if 0  // Test the use of ffmpeg.
#define HH_NO_IMAGE_WIC
#define HH_NO_IMAGE_LIBS
#endif

#if !defined(HH_NO_IMAGE_WIC) && defined(_MSC_VER)  // mingw doesn't recognize <wincodecsdk.h>.
#define HH_IMAGE_HAVE_WIC
#endif

#if defined(HH_IMAGE_HAVE_WIC) && !defined(HH_IMAGE_LIBS_TOO) && !defined(HH_NO_IMAGE_LIBS)
#define HH_NO_IMAGE_LIBS
#endif

#if !defined(HH_NO_IMAGE_LIBS)
#define HH_IMAGE_HAVE_LIBS
#endif

//----------------------------------------------------------------------------

inline void swap(Image& l, Image& r) noexcept {
  using std::swap;
  swap(implicit_cast<Image::base&>(l), implicit_cast<Image::base&>(r));
  swap(l._attrib, r._attrib);
  swap(l._silent_io_progress, r._silent_io_progress);
}

}  // namespace hh

#endif  // MESH_PROCESSING_LIBHH_IMAGE_H_
