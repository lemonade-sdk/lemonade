import React from 'react';

interface ImagePreviewListProps {
  images: string[];
  onRemove: (index: number) => void;
  altPrefix?: string;
  className?: string;
}

const ImagePreviewList: React.FC<ImagePreviewListProps> = ({
  images,
  onRemove,
  altPrefix = 'Upload',
  className = 'image-preview-container',
}) => {
  if (images.length === 0) return null;

  return (
    <div className={className}>
      {images.map((imageUrl, index) => (
        <div key={index} className="image-preview-item">
          <img src={imageUrl} alt={`${altPrefix} ${index + 1}`} className="image-preview" />
          <button
            className="image-remove-button"
            onClick={() => onRemove(index)}
            title="Remove image"
          >
            ×
          </button>
        </div>
      ))}
    </div>
  );
};

export default ImagePreviewList;
