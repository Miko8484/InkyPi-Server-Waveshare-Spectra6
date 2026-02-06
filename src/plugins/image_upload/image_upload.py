from plugins.base_plugin.base_plugin import BasePlugin
from PIL import Image
import logging
import random
import os

logger = logging.getLogger(__name__)


class ImageUpload(BasePlugin):
    def open_image(self, img_index: int, image_locations: list) -> Image:
        if not image_locations:
            raise RuntimeError("No images provided.")
        # Open the image using Pillow
        try:
            image = Image.open(image_locations[img_index])
        except Exception as e:
            logger.error(f"Failed to read image file: {str(e)}")
            raise RuntimeError("Failed to read image file.")
        return image


    def generate_image(self, settings, device_config) -> Image:
        # Get the current index from the device json
        img_index = settings.get("image_index", 0)
        image_locations = settings.get("imageFiles[]")

        if img_index >= len(image_locations):
            # Prevent Index out of range issues when file list has changed
            img_index = 0

        if settings.get('randomize') == "true":
            img_index = random.randrange(0, len(image_locations))
            image = self.open_image(img_index, image_locations)
        else:
            image = self.open_image(img_index, image_locations)
            img_index = (img_index + 1) % len(image_locations)

        # Write the new index back to the device json
        settings['image_index'] = img_index

        # Image is already resized and rotated on upload
        return image

    def cleanup(self, settings):
        """Delete all uploaded image files associated with this plugin instance."""
        image_locations = settings.get("imageFiles[]", [])
        if not image_locations:
            return

        for image_path in image_locations:
            if os.path.exists(image_path):
                try:
                    os.remove(image_path)
                    logger.info(f"Deleted uploaded image: {image_path}")
                except Exception as e:
                    logger.warning(f"Failed to delete uploaded image {image_path}: {e}")
