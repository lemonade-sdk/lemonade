import React from 'react';

interface ComboboxProps {
  optionsList: any[];
  onChangeFunc: (e: any) => void;
  defaultValue: string;
  position?: string;
}

const Combobox: React.FC<ComboboxProps> = React.memo(function Combobox({ optionsList, onChangeFunc, defaultValue, position }) {
  const [datalistOpen, setDatalistlistOpen] = React.useState<boolean>(false);
  const [filteredList, setFilteredList] = React.useState<string[]>(optionsList);
  const comboboxRef = React.useRef<HTMLDivElement>(null);

  const updateInputFromList = (e: any) => {
    let opt = e.target.dataset.option;
    if (opt) onChangeFunc(opt);
    setDatalistlistOpen(!datalistOpen);
  }

  const handleInputChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    if (!datalistOpen) setDatalistlistOpen(true);
    let newVoiceList = e.target.value == '' ? optionsList : filteredList.filter((word) => word.startsWith(e.target.value));
    onChangeFunc(e.target.value);
    setFilteredList(newVoiceList);
  }

  const handleClickOutside = (e: any) => {
    if (comboboxRef.current && !comboboxRef.current.contains(e.target)) {
      setDatalistlistOpen(false);
    }
  };

  React.useEffect(() => {
    document.addEventListener("mousedown", handleClickOutside);
    document.addEventListener("touchstart", handleClickOutside);

    return () => {
      document.removeEventListener("mousedown", handleClickOutside);
      document.removeEventListener("touchstart", handleClickOutside);
    };
  }, []);

  return (
    <div className="form-combobox-container" ref={comboboxRef}>
      <input
        className="form-combobox"
        value={defaultValue}
        onChange={handleInputChange}
        onClick={() => setDatalistlistOpen(!datalistOpen)}
        placeholder='Select a voice...'
      />
      {
        datalistOpen && (filteredList.length > 0) && (
            <ul id="voiceOptionsList" className={position == 'top' ? `form-datalist form-datalist-top` : `form-datalist form-datalist-bottom`}>
            {filteredList.sort().map((voice: string, index: number) => {
              return <li key={index} data-option={voice} onClick={updateInputFromList} className="form-datalist-option">{voice}</li>;
            })}
          </ul>
          )
      }
    </div>
  )
}, (prevProps, nextProps) => {
  return ((nextProps.defaultValue === prevProps.defaultValue));
});

export default Combobox;
