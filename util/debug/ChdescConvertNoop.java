import java.io.DataInput;
import java.io.IOException;

class ChdescConvertNoopFactory extends ModuleOpcodeFactory
{
	public ChdescConvertNoopFactory(DataInput input)
	{
		super(input, KDB_CHDESC_CONVERT_NOOP, "KDB_CHDESC_CONVERT_NOOP");
		addParameter("chdesc", 4);
	}
	
	public ChdescConvertNoop readChdescConvertNoop() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescConvertNoop();
	}
}

public class ChdescConvertNoop extends Opcode
{
	public ChdescConvertNoop(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescConvertNoopFactory(input);
	}
}
