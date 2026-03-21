import React from 'react';

interface ComboboxProps {
  optionsList: string[];
  onChangeFunc: (e: string) => void;
  defaultValue: string;
  placeholder?: string;
  useDefault?: boolean;
  position?: 'top' | 'bottom';
}

const Combobox: React.FC<ComboboxProps> = React.memo(function Combobox({ optionsList, onChangeFunc, defaultValue, placeholder, position, useDefault }) {
  const [datalistOpen, setDatalistOpen] = React.useState<boolean>(false);
  const [filteredList, setFilteredList] = React.useState<string[]>(optionsList);
  const comboboxRef = React.useRef<HTMLDivElement>(null);
  const [selectedVoice, setSelectedVoice] = React.useState<string>(defaultValue);

  const updateInputFromList = (e: any) => {
    let opt = e.target.dataset.option;
    if (opt) onChangeFunc(opt);
    setSelectedVoice(opt);
    setDatalistOpen(false);
  }

  const handleInputChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    if (!datalistOpen) setDatalistOpen(true);
    let newList = e.target.value == '' ? optionsList : optionsList.filter((word) => word.startsWith(e.target.value));
    onChangeFunc(e.target.value);
    setFilteredList(newList);

    if(optionsList.includes(e.target.value)) {
      setSelectedVoice(e.target.value);
    }
  }

  const handleClickOutside = (e: any) => {
    if (comboboxRef.current && !comboboxRef.current.contains(e.target)) {
      setDatalistOpen(false);
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
        className={`form-combobox ${typeof useDefault != "undefined" && (useDefault ? "combobox-default" : "combobox-input-selected")}`}
        value={defaultValue}
        onChange={handleInputChange}
        onClick={() => setDatalistOpen(!datalistOpen)}
        placeholder={ placeholder ? placeholder : "Select an option..."}
      />
      {
        datalistOpen && (filteredList.length > 0) && (
            <ul className={position == 'top' ? `form-datalist form-datalist-top` : `form-datalist form-datalist-bottom`}>
            {filteredList.sort().map((option: string, index: number) => {
              return <li key={index} data-option={option} onClick={updateInputFromList} className={`form-datalist-option ${ option === selectedVoice ? ' selected' : ''}`}>{option}</li>;
            })}
          </ul>
          )
      }
    </div>
  )
});

export default Combobox;
