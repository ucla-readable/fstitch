import java.io.DataInput;
import java.io.IOException;

public class ChdescMove extends Opcode
{
	public ChdescMove(int chdesc, int destination, int target, short offset)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_MOVE, "KDB_CHDESC_MOVE", ChdescMove.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("destination", 4);
		factory.addParameter("target", 4);
		factory.addParameter("offset", 2);
		return factory;
	}
}
