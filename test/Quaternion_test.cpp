// -*- C++ -*-  Copyright (c) Microsoft Corporation; see license.txt
#include "libHh/Quaternion.h"

#include "libHh/Advanced.h"  // clone()
#include "libHh/RangeOp.h"   // round_elements()
using namespace hh;

static Frame round(Frame frame) {
  const int nrows = 3;  // or 4
  for_int(row, nrows) round_elements(frame[row], 1e4f);
  return frame;
}

static Quaternion round(Quaternion q) {
  round_elements(q.access_private());
  return q;
}

int main() {
  {
    Quaternion q1(Vector(1.f, 2.f, 3.f), TAU / 4);
    SHOW(q1);
    SHOW(pow(q1, .28f));
    SHOW(slerp(Quaternion(Vector(0.f, 0.f, 0.f), 0.f), q1, .28f));
    SHOW(exp(log(q1) * .28f));
    SHOW(round_elements(clone(q1.axis())));
    SHOW(q1.angle());
    SHOW(round(pow(q1, .25f) * pow(q1, .75f)));
    Frame frame = to_Frame(q1);
    SHOW(round(frame));
    Frame frame_half = to_Frame(pow(q1, .5f));
    SHOW(frame_half);
    SHOW(round(frame_half * frame_half));
    SHOW(round(pow(frame, .5f)));
    Quaternion qq(pow(pow(frame, .25f), 4.f));
    // SHOW(qq);  // rounding differences
    SHOW(qq.angle());
    SHOW(round_elements(clone(qq.axis())));
    frame = round(frame);
    SHOW(Quaternion(frame));
    SHOW(round(to_Frame(q1 * inverse(q1))));
  }
  {
    // Note: pow(qi, f) == slerp(Quaternion(Vector(0.f, 0.f, 0.f), 0.f), qi, f) == exp(log(qi) * f)
    Quaternion qi(Vector(3.f, 7.f, 11.f), TAU / 14);
    float f = 2.f / 3.f;
    SHOW(f);
    SHOW(qi);
    SHOW(pow(qi, f));
    SHOW(slerp(Quaternion(Vector(0.f, 0.f, 0.f), 0.f), qi, f));
    SHOW(exp(log(qi) * f));
    Quaternion qo = pow(qi, f);
    SHOW(qi.axis());
    SHOW(qo.axis());
    SHOW(qi.angle());
    SHOW(qo.angle());
  }
}
