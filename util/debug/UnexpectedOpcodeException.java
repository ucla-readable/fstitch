public class UnexpectedOpcodeException extends BadInputException
{
	public final short opcodeNumber;
	
	public UnexpectedOpcodeException(short opcodeNumber, int offset)
	{
		super("Unexpected opcode: " + Opcode.hex(opcodeNumber), offset);
		this.opcodeNumber = opcodeNumber;
	}
}
