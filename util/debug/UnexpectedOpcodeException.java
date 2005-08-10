public class UnexpectedOpcodeException extends BadInputException
{
	private final short opcodeNumber;
	
	public UnexpectedOpcodeException(short opcodeNumber)
	{
		super("Unexpected opcode: " + Opcode.hex(opcodeNumber));
		this.opcodeNumber = opcodeNumber;
	}
	
	public short getOpcodeNumber()
	{
		return opcodeNumber;
	}
}