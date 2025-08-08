from ultralytics import YOLO
import os
import logging

# Configure logging
logging.basicConfig(level=logging.INFO)

class YOLODetector:
    """
    A wrapper for the YOLO object detection model using ultralytics library.
    """
    def __init__(self, model_name="yolo11n.pt"):
        """
        Initializes the YOLODetector.

        Args:
            model_name (str): The YOLO model name (e.g., 'yolo11n.pt', 'yolo11s.pt').
        """
        self.model_name = model_name
        try:
            self.model = YOLO(model_name)
            logging.info(f"Successfully loaded YOLO model: {model_name}")
        except Exception as e:
            self.model = None
            logging.error(f"Failed to load YOLO model {model_name}: {e}")

    def predict(self, image_path):
        """
        Predicts objects in an image using the YOLO model.

        Args:
            image_path (str): The path to the image file.

        Returns:
            list: A list of detected object names (strings only).
        """
        if not self.model:
            logging.error("YOLO model not initialized. Cannot make predictions.")
            return []
            
        if not os.path.exists(image_path):
            logging.error(f"Image path does not exist: {image_path}")
            return []

        try:
            # Run inference
            results = self.model(image_path)
            
            detected_objects = []
            
            # Extract detected object names from results
            for result in results:
                if result.boxes is not None:
                    for box in result.boxes:
                        # Get class ID and convert to class name
                        class_id = int(box.cls)
                        class_name = self.model.names[class_id]
                        confidence = float(box.conf)
                        
                        # Only include detections with reasonable confidence
                        if confidence > 0.5:
                            detected_objects.append(class_name)
            
            # Remove duplicates while preserving order
            detected_objects = list(dict.fromkeys(detected_objects))
            
            logging.info(f"Prediction complete. Detected: {detected_objects}")
            return detected_objects

        except Exception as e:
            logging.error(f"An error occurred during YOLO prediction: {e}")
            return [] 