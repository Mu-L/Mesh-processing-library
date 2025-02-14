// -*- C++ -*-  Copyright (c) Microsoft Corporation; see license.txt
#ifndef MESH_PROCESSING_LIBHH_QUATERNION_H_
#define MESH_PROCESSING_LIBHH_QUATERNION_H_

#include "libHh/Geometry.h"
#include "libHh/MathOp.h"

namespace hh {

// A quaternion is a 4D algebraic representation.
// Unit quaternions represent the rotation group SO(3) and thus represent orientations and rotations of objects in 3D,
//  e.g. a transformation from one orthogonal Frame to another (with the same handedness and ignoring origins).
class Quaternion {
 public:
  Quaternion() { zero(); }
  Quaternion(const Quaternion& q) = default;
  explicit Quaternion(const Frame& frame);  // mapping from Frame::identity() to frame; origin frame.p() is ignored.
  explicit Quaternion(const Vector& axis, float angle);
  explicit Quaternion(const Vector& vf, const Vector& vt);  // resulting quaternion is 2 times rotation from vf to vt!
  Quaternion& operator=(const Quaternion&) = default;
  void zero() { _c[0] = _c[1] = _c[2] = 0.f, _c[3] = 1.f; }
  // extraction
  void angle_axis(float& angle, Vector& axis) const;
  float angle() const;
  Vector axis() const;  // axis will be zero vector if angle == 0
  bool is_unit() const;
  friend Quaternion operator*(const Quaternion& q1, const Quaternion& q2);
  friend Quaternion inverse(const Quaternion& qi);
  friend Frame to_Frame(const Quaternion& q);
  friend Quaternion pow(const Quaternion& qi, float e);
  friend Frame pow(const Frame& frame, float e);
  friend Vector log(const Quaternion& q);
  friend Quaternion exp(const Vector& v);
  friend Quaternion slerp(const Quaternion& q0, const Quaternion& q1, float t);
  friend Quaternion squad(const Quaternion& q0, const Quaternion& a, const Quaternion& b, const Quaternion& q1,
                          float t);
  friend Quaternion squadseg(const Quaternion* qb, const Quaternion& q0, const Quaternion& q1, const Quaternion* qa,
                             float t);
  friend std::ostream& operator<<(std::ostream& os, const Quaternion& q);

 public:  // discouraged
  Vec4<float>& access_private() { return _c; }
  const Vec4<float>& access_private() const { return _c; }

 private:
  Vec4<float> _c;
  Quaternion(float x, float y, float z, float w) : _c(x, y, z, w) {}
  float& operator[](int i) { return _c[i]; }
  const float& operator[](int i) const { return _c[i]; }
  friend float mag2(const Quaternion& q) { return mag2(q._c); }
  friend float mag(const Quaternion& q) { return mag(q._c); }
  friend float dot(const Quaternion& q1, const Quaternion& q2) { return dot(q1._c, q2._c); }
  void normalize() { _c *= 1.f / assertx(mag(_c)); }
};

Quaternion operator*(const Quaternion& q1, const Quaternion& q2);
Quaternion inverse(const Quaternion& qi);
inline Quaternion& operator*=(Quaternion& q1, const Quaternion& q2) { return q1 = q1 * q2; }
Frame to_Frame(const Quaternion& q);  // Frame origin is set to zero!
Quaternion pow(const Quaternion& qi, float e);
Frame pow(const Frame& frame, float e);  // Power of Frame
Vector log(const Quaternion& q);         // ? log(Quaternion) == ?what type.   a Vector? vo[3] == 0!
Quaternion exp(const Vector& v);         // ? meaning of exponentiation

// NOTE:    pow(qi, e) == slerp(Quaternion(Vector(0.f, 0.f, 0.f), 0.f), qi, e) == exp(log(qi) * e)
// spherical linear interpolation of unit quaternion.
// t:[0..1] slerp(0) == q0, slerp(1) == q1
Quaternion slerp(const Quaternion& q0, const Quaternion& q1, float t);

// Spherical cubic interpolation of unit quaternion;  t:[0..1]  squad(0) = q0, squad(1) = q1,
//  a and b are intermediate quaternions to form Bezier-like curve.
Quaternion squad(const Quaternion& q0, const Quaternion& a, const Quaternion& b, const Quaternion& q1, float t);

// Hermite-like segment.  Again: squadseg(0) = q0, squadseg(1) = q1 but simulate squadseg(-1) = qb, squadseg(2) = qa.
Quaternion squadseg(const Quaternion* qb, const Quaternion& q0, const Quaternion& q1, const Quaternion* qa, float t);

std::ostream& operator<<(std::ostream& os, const Quaternion& q);

//----------------------------------------------------------------------------

// Frame origin frame.p() is ignored
inline Quaternion::Quaternion(const Frame& frame) {
  const float tr = frame[0][0] + frame[1][1] + frame[2][2];
  float s;
  if (tr > 0) {
    s = my_sqrt(tr + 1.f);
    _c[3] = s * .5f;
    s = .5f / s;
    _c[0] = (frame[1][2] - frame[2][1]) * s;
    _c[1] = (frame[2][0] - frame[0][2]) * s;
    _c[2] = (frame[0][1] - frame[1][0]) * s;
  } else {
    int i = 0, j, k;
    if (frame[1][1] > frame[0][0]) i = 1;
    if (frame[2][2] > frame[i][i]) i = 2;
    j = mod3(i + 1);
    k = mod3(i + 2);
    s = my_sqrt((frame[i][i] - (frame[j][j] + frame[k][k])) + 1.f);
    _c[i] = s * .5f;
    s = .5f / s;
    _c[3] = (frame[j][k] - frame[k][j]) * s;
    _c[j] = (frame[i][j] + frame[j][i]) * s;
    _c[k] = (frame[i][k] + frame[k][i]) * s;
  }
  normalize();  // optional; just to be sure
}

inline Quaternion::Quaternion(const Vector& axis, float angle) {
  float a = mag(axis);
  a = a ? std::sin(angle * .5f) / a : 1.f;
  _c[0] = axis[0] * a;
  _c[1] = axis[1] * a;
  _c[2] = axis[2] * a;
  _c[3] = std::cos(angle * .5f);
}

inline Quaternion::Quaternion(const Vector& vf, const Vector& vt) {
  Vector vc = cross(vf, vt);
  _c[0] = vc[0];
  _c[1] = vc[1];
  _c[2] = vc[2];
  _c[3] = dot(vf, vt);
}

inline void Quaternion::angle_axis(float& angle, Vector& axis) const {
  ASSERTXX(is_unit());
  // angle = my_acos(_c[3]) * 2.f;
  // float a = std::sin(angle * .5f);
  // a = a ? 1.f / a : 1.f;
  // axis = _c.head<3>() * a;
  const float xyz = mag(_c.head<3>());
  angle = my_asin(xyz) * 2.f;
  const float a = xyz ? 1.f / xyz : 1.f;
  axis = _c.head<3>() * a;
}

inline float Quaternion::angle() const {
  ASSERTXX(is_unit());
  if (0) {
    return my_acos(_c[3]) * 2.f;
  } else {
    const float xyz = mag(_c.head<3>());
    return my_asin(xyz) * 2.f;
  }
}

inline Vector Quaternion::axis() const {
  ASSERTXX(is_unit());
  // float a = my_sqrt(1.f - _c[3] * _c[3]);  // == std::sin(angle() * .5)
  // a = a ? 1.f / a : 1.f;
  float xyz = mag(_c.head<3>());
  float a = xyz ? 1.f / xyz : 1.f;
  return _c.head<3>() * a;
}

// Frame origin is set to zero!
inline Frame to_Frame(const Quaternion& q) {
  float xs, ys, zs, wx, wy, wz, xx, xy, xz, yy, yz, zz;
  ASSERTXX(q.is_unit());
  xs = q[0] + q[0];
  ys = q[1] + q[1];
  zs = q[2] + q[2];
  wx = q[3] * xs;
  wy = q[3] * ys;
  wz = q[3] * zs;
  xx = q[0] * xs;
  xy = q[0] * ys;
  xz = q[0] * zs;
  yy = q[1] * ys;
  yz = q[1] * zs;
  zz = q[2] * zs;
  return Frame(Vector(1 - yy - zz, xy + wz, xz - wy), Vector(xy - wz, 1 - xx - zz, yz + wx),
               Vector(xz + wy, yz - wx, 1 - xx - yy), Point(0.f, 0.f, 0.f));
}

inline bool Quaternion::is_unit() const { return abs(mag2(*this) - 1.f) <= 1e-6f; }

inline Quaternion operator*(const Quaternion& q1, const Quaternion& q2) {
  ASSERTXX(q1.is_unit() && q2.is_unit());
  Quaternion q(q1[3] * q2[0] + q1[0] * q2[3] + q1[1] * q2[2] - q1[2] * q2[1],
               q1[3] * q2[1] + q1[1] * q2[3] + q1[2] * q2[0] - q1[0] * q2[2],
               q1[3] * q2[2] + q1[2] * q2[3] + q1[0] * q2[1] - q1[1] * q2[0],
               q1[3] * q2[3] - q1[0] * q2[0] - q1[1] * q2[1] - q1[2] * q2[2]);
  q.normalize();
  return q;
}

inline Quaternion inverse(const Quaternion& qi) {
  ASSERTXX(qi.is_unit());
  return Quaternion(-qi[0], -qi[1], -qi[2], qi[3]);
}

inline Quaternion pow(const Quaternion& qi, float e) {
  Quaternion q;
  ASSERTXX(qi.is_unit());
  if (0) {
    float wo = qi[3];
    float ango = my_acos(wo) * 2.f;
    float angn = ango * e;
    float wn = std::cos(angn * .5f);
    float xyzo = my_sqrt(1.f - wo * wo);
    float xyzn = my_sqrt(1.f - wn * wn);
    q[3] = wn;
    float a = xyzo ? xyzn / xyzo : 1.f;
    for_int(i, 3) q[i] = qi[i] * a;
    q.normalize();
  } else {
    // 2007-06-15 much more numerically stable
    // The important case is small angles, for which all the precision is in qi[0..2].
    float xyzo = mag(qi._c.head<3>());
    float ango = my_asin(xyzo) * 2.f;
    float angn = ango * e;
    float xyzn = std::sin(angn * .5f);
    float a = xyzo ? xyzn / xyzo : 1.f;
    for_int(i, 3) q[i] = qi[i] * a;
    q[3] = my_sqrt(1.f - mag2(q._c.head<3>()));
  }
  return q;
}

inline Frame pow(const Frame& frame, float v) {
  Frame frame2 = to_Frame(pow(Quaternion(frame), v));
  frame2.p() = frame.p() * v;
  return frame2;
}

inline Vector log(const Quaternion& qi) {
  ASSERTXX(qi.is_unit());
  float scale = mag(qi._c.head<3>());
  assertx(scale || qi[3]);
  const float theta = std::atan2(scale, qi[3]);
  if (scale > 0.f) scale = theta / scale;
  const Vector v = qi._c.head<3>() * scale;
  return v;
}

inline Quaternion exp(const Vector& v) {
  Quaternion q;
  const float theta = mag(v);
  const float scale = theta > 1e-6f ? std::sin(theta) / theta : 1.f;
  q._c.head<3>() = v * scale;
  q[3] = std::cos(theta);
  return q;
}

inline Quaternion slerp(const Quaternion& q0, const Quaternion& q1, float t) {
  float omega, cosom, sinom, sclp, sclq;
  Quaternion q;
  ASSERTXX(q0.is_unit() && q1.is_unit());
  cosom = dot(q0, q1);
  if (1 + cosom < 1e-6f) {  // ends nearly opposite
    q[0] = -q0[1];
    q[1] = q0[0];
    q[2] = -q0[3];
    q[3] = q0[2];
    sclp = std::sin((.5f - t) * (TAU / 2));
    sclq = std::sin(t * (TAU / 2));
    for_int(i, 3) q[i] = sclp * q0[i] + sclq * q[i];
  } else if (1 - cosom < 1e-6f) {  // ends very close
    sclp = 1.f - t;
    sclq = t;
    for_int(i, 4) q[i] = sclp * q0[i] + sclq * q1[i];
  } else {  // usual case
    omega = my_acos(cosom);
    sinom = std::sin(omega);
    sclp = std::sin((1.f - t) * omega) / sinom;
    sclq = std::sin(t * omega) / sinom;
    for_int(i, 4) q[i] = sclp * q0[i] + sclq * q1[i];
  }
  q.normalize();  // just to make sure
  return q;
}

inline Quaternion squad(const Quaternion& q0, const Quaternion& a, const Quaternion& b, const Quaternion& q1,
                        float t) {
  return slerp(slerp(q0, q1, t), slerp(a, b, t), 2 * (1 - t) * t);
}

inline Quaternion squadseg(const Quaternion* qb, const Quaternion& q0, const Quaternion& q1, const Quaternion* qa,
                           float t) {
  Quaternion a = q0, b = q1;
  if (qb) a = q0 * exp((log(inverse(q0) * q1) + log(inverse(q0) * *qb)) * -.25f);
  if (qa) b = q1 * exp((log(inverse(q1) * *qa) + log(inverse(q1) * q0)) * -.25f);
  return squad(q0, a, b, q1, t);
}

inline std::ostream& operator<<(std::ostream& os, const Quaternion& q) {
  if (0) ASSERTXX(q.is_unit());  // coefficients may have been rounded, e.g. in Quaternion_test.cpp
  return os << "Quaternion(" << q[0] << ", " << q[1] << ", " << q[2] << ", " << q[3] << ")";
}

}  // namespace hh

#endif  // MESH_PROCESSING_LIBHH_QUATERNION_H_
