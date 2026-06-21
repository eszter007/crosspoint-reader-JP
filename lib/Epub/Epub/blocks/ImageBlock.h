#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  // When set, the image is rendered rotated 90° (for aspect-mismatched images
  // on a dedicated page). reserveMargin is inset on all edges so it never
  // overlaps the status bar regardless of which edge it maps to after rotation.
  void setRotated(bool r, int16_t reserveMargin) {
    rotated = r;
    reserveMargin_ = reserveMargin;
  }
  bool isRotated() const { return rotated; }

  bool imageExists() const;

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  int16_t width;
  int16_t height;
  bool rotated = false;
  int16_t reserveMargin_ = 0;
};
