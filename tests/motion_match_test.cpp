#include "motion_match.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>
using namespace feverscaler;
using Matrix = std::array<float,16>;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
static Matrix At(float x, float y=0, float z=0) {
  Matrix m{}; m[0]=m[5]=m[10]=m[15]=1; m[12]=x; m[13]=y; m[14]=z; return m;
}
static std::vector<int32_t> Match(const std::vector<Matrix>& current, const std::vector<Matrix>& previous) {
  std::vector<const float*> c,p;
  for (auto& m:current) c.push_back(m.data());
  for (auto& m:previous) p.push_back(m.data());
  std::vector<int32_t> matches;
  MatchMotionInstances(c,p,5.0f,matches);
  return matches;
}
int main() {
  // Identical car geometry, reordered draw batches: each car must retain its own history.
  std::vector<Matrix> previous{At(0),At(20),At(40),At(60)};
  auto result=Match({At(60.5f),At(.5f),At(40.5f),At(20.5f)},previous);
  CHECK((result==std::vector<int32_t>{3,0,2,1}));
  // Culling, additions and a different batch/instance count cannot shift later cars' identities.
  result=Match({At(40.5f),At(100),At(.5f)},previous);
  CHECK((result==std::vector<int32_t>{2,-1,0}));
  // A renderer may submit the same convoy front-to-back or back-to-front.
  std::vector<Matrix> current{At(.7f),At(20.7f),At(40.7f),At(60.7f)};
  do {
    result=Match(current,previous);
    for (size_t i=0;i<current.size();++i) {
      CHECK(result[i]>=0);
      CHECK(std::fabs(current[i][12]-previous[result[i]][12]-.7f)<1e-4f);
    }
  } while (std::next_permutation(current.begin(),current.end(),[](const Matrix&a,const Matrix&b){return a[12]<b[12];}));
  // Uncertain pairing is discarded, and one old instance cannot become two moving objects.
  CHECK((Match({At(0)}, {At(-1),At(1)})==std::vector<int32_t>{-1}));
  CHECK((Match({At(.1f),At(.2f)}, {At(0)})==std::vector<int32_t>{-1,-1}));
  CHECK((Match({At(0),At(.5f)}, {At(0)})==std::vector<int32_t>{0,-1}));
  CHECK((Match({At(4.9f)}, {At(0),At(10)})==std::vector<int32_t>{-1}));
  CHECK((Match({At(100)}, {At(0)})==std::vector<int32_t>{-1}));
  // Negative coordinates and vertical motion cross spatial-cell boundaries correctly.
  CHECK((Match({At(-10.1f,-.2f,.5f)}, {At(-9.9f,0,0)})==std::vector<int32_t>{0}));
  Matrix bad=At(0); bad[12]=std::numeric_limits<float>::quiet_NaN();
  CHECK((Match({bad}, {At(0)})==std::vector<int32_t>{-1}));
  CHECK((Match({bad}, {bad})==std::vector<int32_t>{-1}));
  CHECK((Match({At(0)}, {})==std::vector<int32_t>{-1}));
  // Matrix rotation, not only translation, remains available to the replay shader.
  Matrix rotated=At(0); rotated[0]=rotated[5]=0; rotated[1]=1; rotated[4]=-1;
  CHECK((Match({rotated}, {At(0)})==std::vector<int32_t>{0}));
  // Large unchanged and reordered batches: no fixed +/-8-slot assumption.
  previous.clear();
  for (int i=0;i<16384;++i) previous.push_back(At((float)(i%128)*20,(float)(i/128)*20));
  result=Match(previous,previous);
  for (int i=0;i<16384;++i) CHECK(result[i]==i);
  current=previous; std::reverse(current.begin(),current.end());
  result=Match(current,previous);
  for (int i=0;i<16384;++i) CHECK(result[i]==16383-i);
  std::puts("motion history: reorder, culling, additions, ambiguity and large batches passed");
}
