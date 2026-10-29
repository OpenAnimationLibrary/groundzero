#include "movierenderqueue.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(1);
}

void failureUnblocksSuccessfulFrames() {
  MovieRenderQueue<int> queue;
  for (double frame : {12.5, 13.0, 13.5, 14.0}) queue.addFrame(frame);

  // Later render workers finish before the first worker fails. In particular,
  // the requested range does not contain frame zero.
  queue.push(13.5, 135);
  queue.push(14.0, 140);
  MovieRenderQueue<int>::Result result;
  double frame = -1;
  require(!queue.takeNext(frame, result), "saved past an unfinished frame");

  // A render task may contain several identical exposures. Every failed
  // exposure must advance the ordered writer, without inventing frame zero.
  queue.fail(12.5, L"render failure");
  queue.fail(13.0, L"render failure");
  const std::vector<double> expected{12.5, 13.0, 13.5, 14.0};
  std::vector<int> written;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    require(queue.takeNext(frame, result), "failure left the queue blocked");
    require(frame == expected[i], "writer received the wrong frame");
    require(result.failed == (i < 2), "good frame was marked failed");
    if (result.failed)
      require(result.error == L"render failure", "lost failure diagnostic");
    else
      written.push_back(result.payload);
  }
  require(written == std::vector<int>({135, 140}),
          "successful buffered frames were discarded after a render failure");
  require(queue.complete() && !queue.takeNext(frame, result),
          "completed sequence still has work");

  queue.fail(12.5, L"duplicate failure");
  queue.push(14.0, 999);
  queue.fail(0.0, L"not requested");
  require(!queue.takeNext(frame, result), "duplicate callback repeated a frame");
  require(!queue.take(0.0, result), "invented a frame outside the requested range");
}

void independentClustersKeepOwnership() {
  MovieRenderQueue<int> queue;
  for (double frame : {1.0, 2.0, 3.0}) queue.addFrame(frame);
  queue.push(1.0, 10);
  queue.push(2.0, 20);
  queue.push(3.0, 30);
  MovieRenderQueue<int>::Result first, second;
  require(queue.take(3.0, first) && first.payload == 30,
          "independent cluster unnecessarily waited for earlier frames");
  require(queue.take(1.0, first) && queue.take(2.0, second),
          "independent worker lost part of its cluster");
  require(first.payload == 10 && second.payload == 20,
          "cluster payloads changed while another worker drained");
  double frame;
  require(queue.complete() && !queue.takeNext(frame, first),
          "out-of-order consumption failed to complete");
}

void failureDoesNotOverwriteFrameZero() {
  MovieRenderQueue<int> queue;
  for (double frame : {0.0, 1.0, 2.0}) queue.addFrame(frame);
  queue.push(2.0, 20);
  queue.push(0.0, 10);
  queue.fail(1.0, L"failed middle frame");
  MovieRenderQueue<int>::Result result;
  double frame;
  require(queue.takeNext(frame, result) && frame == 0.0 &&
              !result.failed && result.payload == 10,
          "failure replaced the successful frame-zero raster");
  require(queue.takeNext(frame, result) && frame == 1.0 && result.failed,
          "middle failure was attributed to the wrong frame");
  require(queue.takeNext(frame, result) && frame == 2.0 &&
              !result.failed && result.payload == 20 && queue.complete(),
          "middle failure prevented the final successful frame from saving");
}

void cancellationReleasesBufferedFrames() {
  MovieRenderQueue<std::shared_ptr<int>> queue;
  queue.addFrame(20.0);
  queue.addFrame(21.0);
  std::weak_ptr<int> pending;
  {
    auto raster = std::make_shared<int>(42);
    pending = raster;
    queue.push(21.0, raster);
  }
  require(!pending.expired(), "queue did not retain a pending raster");
  queue.discardPending();
  require(pending.expired(), "cancellation retained buffered rasters");
  require(!queue.complete(), "discarded frames were reported as complete");
}

}  // namespace

int main() {
  failureUnblocksSuccessfulFrames();
  independentClustersKeepOwnership();
  failureDoesNotOverwriteFrameZero();
  cancellationReleasesBufferedFrames();
  std::cout << "movie render queue failure and ordering checks passed\n";
  return 0;
}
