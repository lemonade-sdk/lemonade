import React, { ReactElement } from 'react';

export const PAUSED = 0;
export const LOADING = 1;
export const PLAYING = 2;

interface  AudioButtonProps {
  textMessage: any;
  buttonIndex: number;
  onClickFunction: (message: any, buttonIndex: number) => void;
  buttonContext: {buttonId: number, audioState: number};
}

const AudioButton: React.FC<AudioButtonProps> = React.memo(function AudioButton({ textMessage, buttonIndex, onClickFunction, buttonContext}) {
  const LoadingIcon = () => {
    return (
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 18 18" width="18px" height="18px"  style={{opacity:1}}>
        <circle cx="3" cy="9" r="2" fill="currentColor">
          <animate id="SVG9IgbRbsl" attributeName="r" begin="0;SVGFUNpCWdG.end-0.35s" dur="0.95s" values="3;.2;3"/>
        </circle>
        <circle cx="9" cy="9" r="2" fill="currentColor">
          <animate attributeName="r" begin="SVG9IgbRbsl.end-0.7s" dur="0.95s" values="3;.2;3"/>
        </circle>
        <circle cx="16" cy="9" r="2" fill="currentColor">
          <animate id="SVGFUNpCWdG" attributeName="r" begin="SVG9IgbRbsl.end-0.55s" dur="0.95s" values="3;.2;3"/>
        </circle>
      </svg>
    )
  }

  const PausedIcon = () => {
    return (
      <svg fill="#000000" width="18px" height="18px" viewBox="-11.5 0 32 32" version="1.1" xmlns="http://www.w3.org/2000/svg">
        <path d="M8.2 22.48c-0.48 0-0.84-0.36-0.84-0.84v-11.28c0-0.48 0.36-0.84 0.84-0.84s0.84 0.36 0.84 0.84v11.32c0 0.44-0.36 0.8-0.84 0.8zM0.84 22.48c-0.48 0-0.84-0.36-0.84-0.84v-11.28c0-0.48 0.36-0.84 0.84-0.84s0.84 0.36 0.84 0.84v11.32c-0.040 0.44-0.36 0.8-0.84 0.8z"></path>
      </svg>
    )
  }

  const PlayingIcon = () => {
    return(
      <svg fill="#000000" width="18px" height="18px" viewBox="-3.5 0 32 32" version="1.1" xmlns="http://www.w3.org/2000/svg">
        <path d="M13.16 25.46c-0.16 0-0.28-0.040-0.4-0.12l-7.52-4.32h-2.6c-1.44 0-2.64-1.16-2.64-2.6v-4.8c0-1.44 1.2-2.64 2.64-2.64h2.6l7.52-4.32c0.12-0.080 0.28-0.12 0.4-0.12 1.44 0 2.64 1.2 2.64 2.64v13.6c0 1.48-1.16 2.68-2.64 2.68zM2.64 12.66c-0.52 0-0.96 0.44-0.96 0.96v4.8c0 0.52 0.44 0.96 0.96 0.96h2.84c0.16 0 0.28 0.040 0.4 0.12l7.48 4.28c0.44-0.080 0.76-0.48 0.76-0.92v-13.64c0-0.48-0.32-0.84-0.76-0.92l-7.48 4.28c-0.12 0.080-0.28 0.12-0.4 0.12l-2.84-0.040zM18.64 21.297c-0.16 0-0.32-0.040-0.48-0.16-0.4-0.24-0.48-0.8-0.24-1.16 0.8-1.16 1.2-2.52 1.2-3.92 0-1.44-0.44-2.84-1.28-4.040-0.28-0.36-0.16-0.92 0.2-1.16 0.36-0.28 0.92-0.16 1.16 0.2 1.040 1.48 1.56 3.2 1.56 5 0 1.76-0.52 3.44-1.48 4.88-0.080 0.24-0.36 0.36-0.64 0.36zM22.28 23.042c-0.16 0-0.28-0.040-0.44-0.12-0.4-0.24-0.52-0.76-0.28-1.16 1.040-1.72 1.6-3.72 1.6-5.76s-0.56-4.040-1.6-5.76c-0.24-0.4-0.12-0.92 0.28-1.16s0.92-0.12 1.16 0.28c1.2 2 1.84 4.28 1.84 6.64s-0.64 4.64-1.84 6.64c-0.16 0.24-0.44 0.4-0.72 0.4z"></path>
      </svg>
    )
  }

  const renderButtonIcon = () : ReactElement=> {
    if(buttonContext.buttonId == buttonIndex) {
      switch(buttonContext.audioState) {
        case LOADING:
          return <LoadingIcon />;
        case PLAYING:
          return <PausedIcon />;
        default:
          return <PlayingIcon />;  
      }
    } else {
      return <PlayingIcon />; 
    }
  }

  return (
    <div key={buttonIndex} className="message-play-button-container">
      <button className="message-play-button" onClick={() => onClickFunction(textMessage, buttonIndex)} disabled={buttonContext.audioState == LOADING}>{renderButtonIcon()}</button>
    </div>
  );
}, (prevProps, nextProps) => {
  return (nextProps.buttonContext.buttonId === prevProps.buttonContext.buttonId) && (nextProps.buttonContext.audioState === prevProps.buttonContext.audioState) && (nextProps.textMessage == prevProps.textMessage) && (nextProps.onClickFunction == prevProps.onClickFunction);
});

export default AudioButton;