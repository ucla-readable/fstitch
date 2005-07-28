import java.io.DataInput;
import java.io.IOException;

class ChdescSatisfyFactory extends ModuleOpcodeFactory
{
	public ChdescSatisfyFactory(DataInput input)
	{
		super(input, KDB_CHDESC_SATISFY, "KDB_CHDESC_SATISFY");
		addParameter("chdesc", 4);
	}
	
	public ChdescSatisfy readChdescSatisfy() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescSatisfy();
	}
}

public class ChdescSatisfy extends Opcode
{
	public ChdescSatisfy(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescSatisfyFactory(input);
	}
}
